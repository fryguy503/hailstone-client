#include "game/output_file.h"

#include <algorithm>
#include <format>

#include "game/augments.h"
#include "game/eq_strings.h"
#include "game/inventory.h"
#include "game/option_commands.h"
#include "game/spells.h"
#include "net/guild_bank_packets.h"

namespace hailstone::game {

OutputFileKind output_file_kind(std::string_view word) {
    // Cmd_outputfile's stricmp chain (0x9cef78 .. 0x9cef30).
    struct Name {
        const char* word;
        OutputFileKind kind;
    };
    static constexpr Name kNames[] = {{"guild", OutputFileKind::Guild},         {"raid", OutputFileKind::Raid},
                                      {"spellbook", OutputFileKind::Spellbook}, {"inventory", OutputFileKind::Inventory},
                                      {"guildbank", OutputFileKind::GuildBank}, {"realestate", OutputFileKind::RealEstate},
                                      {"guildhall", OutputFileKind::GuildHall}};
    for (const auto& n : kNames)
        if (same_no_case(word, n.word)) return n.kind;
    return OutputFileKind::None;
}

std::string output_file_date(const std::tm& t) {
    return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
                       t.tm_sec);
}

void dump_inventory_item(std::string& out, const net::Item* item, const std::string& label) {
    if (!item) {
        out += label + "\tEmpty\t0\t0\t0\n";
        return;
    }
    std::uint32_t count = item->stackable() ? item->stack_count : 1;
    int slots = item->is_bag() ? item->bag_slots : kAugSockets;
    out += std::format("{}\t{}\t{}\t{}\t{}\n", label, item->name, item->id, count, slots);
    for (int i = 0; i < slots; ++i) {
        const net::Item* child = nullptr;
        if (item->is_bag()) {
            child = item->content(static_cast<std::uint32_t>(i)); // ItemBase_GetChildItem
        } else {
            if (!aug_socket_valid(*item, i) || !aug_socket_visible(*item, i)) continue;
            child = augment_in(*item, i);
        }
        dump_inventory_item(out, child, std::format("{}-Slot{}", label, i + 1));
    }
}

std::string inventory_file(const Inventory& inv, const EqStrings& strings) {
    // The eqstr names of the possessions slots 0..33 (0xa02238).
    static constexpr std::uint32_t kSlotNames[34] = {5776, 559,  560,  561,  559,  562,  563,  564,  565,  566,  566, 567,
                                                     568,  569,  570,  571,  571,  572,  573,  574,  575,  9178, 576, 3488,
                                                     3489, 3490, 3491, 3492, 3493, 3494, 3495, 729,  730,  558};
    std::string out = "Location\tName\tID\tCount\tSlots\n";
    for (int slot = 0; slot < 34; ++slot)
        dump_inventory_item(out, inv.at(net::ItemSlot::possessions(static_cast<std::int16_t>(slot))),
                            strings.format(kSlotNames[slot], {}));
    const std::string bank = strings.format(3496, {}), shared = strings.format(3497, {});
    for (int slot = 0; slot < 24; ++slot)
        dump_inventory_item(out, inv.at({net::kInvBank, static_cast<std::int16_t>(slot), -1, -1, 0, 0}),
                            bank + std::to_string(slot + 1));
    for (int slot = 0; slot < 2; ++slot)
        dump_inventory_item(out, inv.at({net::kInvSharedBank, static_cast<std::int16_t>(slot), -1, -1, 0, 0}),
                            shared + std::to_string(slot + 1));
    return out;
}

std::string spellbook_file(std::span<const std::uint32_t> book,
                           const std::function<const SpellData*(std::uint32_t)>& spell,
                           const std::function<std::uint8_t(const SpellData&)>& level) {
    std::string out;
    for (std::size_t i = 0; i < book.size() && i < 720; ++i) {
        std::uint32_t id = book[i];
        if (!is_spell_id(id)) continue;
        const SpellData* s = spell(id);
        if (s) out += std::format("{}\t{}\n", static_cast<unsigned>(level(*s)), s->name);
    }
    return out;
}

std::string raid_roster_file(const std::vector<std::vector<std::string>>& raid,
                             const std::vector<std::vector<std::string>>& unassigned, const std::string& empty,
                             const std::function<std::string(const std::string&)>& note) {
    auto col = [](const std::vector<std::string>& r, std::size_t c) { return c < r.size() ? r[c] : std::string(); };
    std::string out;
    for (const auto& r : raid) {
        std::string name = col(r, 1);
        if (name.empty() || name == empty) continue;
        out += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", col(r, 0), name, col(r, 2), col(r, 3), col(r, 5), note(name));
    }
    for (const auto& r : unassigned) {
        std::string name = col(r, 1);
        if (name.empty()) continue;
        out += std::format("0\t{}\t{}\t{}\t{}\t{}\n", name, col(r, 2), col(r, 3), col(r, 5), note(name));
    }
    return out;
}

std::string guild_roster_file(const DumpList& members, const DumpList& tribute, const DumpList& notes,
                              std::string_view separator) {
    auto cell = [](const DumpList& l, std::size_t row, int c) {
        return row < l.rows.size() && c < static_cast<int>(l.rows[row].size()) ? l.rows[row][static_cast<std::size_t>(c)]
                                                                              : std::string();
    };
    std::string out;
    for (std::size_t row = 0; row < members.rows.size(); ++row) {
        int n = std::max(members.columns, 1) - 1;
        for (int c = 0; c < n; ++c) {
            out += cell(members, row, c);
            if (c < n - 1) out += separator;
        }
        for (const DumpList* l : {&tribute, &notes}) {
            out += separator;
            int m = std::max(l->columns, 1);
            for (int c = 1; c < m; ++c) {
                out += cell(*l, row, c);
                if (c < m - 1) out += separator;
            }
        }
        out += "\n";
    }
    return out;
}

std::string guild_bank_file(const std::vector<std::optional<net::GuildBankItem>>& main,
                            const std::vector<std::optional<net::GuildBankItem>>& deposit, const EqStrings& strings) {
    std::string out = "Location\tName\tID\tCount\tPermission\n";
    auto line = [&](const net::GuildBankItem& b, const std::string& label) {
        auto p = static_cast<std::uint32_t>(b.permission);
        std::string permission = p == 1 ? b.who_for : p <= 3 ? strings.format(6103 + p, {}) : std::string();
        out += std::format("{}\t{}\t{}\t{}\t{}\n", label, b.name, b.item_id, b.quantity, permission);
    };
    for (std::size_t i = 0; i < main.size() && i < 200; ++i)
        if (main[i]) line(*main[i], std::format("Bank-{:03}", i + 1));
    for (std::size_t i = 0; i < deposit.size() && i < 40; ++i)
        if (deposit[i]) line(*deposit[i], std::format("Deposit-{:02}", i + 1));
    return out;
}

std::string real_estate_file() { return "RealEstateLocation\tRealEstateName\tItemName\tItemOwner\tStatus\tID\tCount\n"; }

} // namespace hailstone::game
