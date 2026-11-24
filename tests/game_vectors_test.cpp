// Test vectors recorded from the original eqgame.exe for
// game rules outside the character and movement mechanics: coin, item links,
// spell CRCs and spell descriptions. Without the
// vectors directory (HAILSTONE_VECTORS_DIR) the checks pass with a message.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <cstdio>

#include "game/item_links.h"
#include "game/merchant.h"
#include "game/spell_description.h"
#include "game/spell_record.h"
#include "game/spells.h"
#include "game_files.h"
#include "json_lite.h"
#include "vector_files.h"

using hailstone::test::Json;
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> vectors_dir() { return hailstone::test::vector_dir(); }

// Runs `check` on every record of `file`; it returns a mismatch text or "".
void each_record(const char* file, const std::function<std::string(const Json& in, const Json& out)>& check) {
    auto dir = vectors_dir();
    std::ifstream f(dir ? *dir / file : fs::path());
    if (!f) {
        MESSAGE(std::string(file) << ": no vectors (HAILSTONE_VECTORS_DIR); skipped");
        return;
    }
    std::string line;
    int n = 0, bad = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        hailstone::test::normalise_recorded_names(line);
        ++n;
        Json r = hailstone::test::parse_json(line);
        std::string why = check(r["in"], r["out"]);
        if (!why.empty() && ++bad <= 10) FAIL_CHECK(std::string(file) << ":" << n << ": " << why);
    }
    MESSAGE(std::string(file) << ": " << n << " records, " << bad << " mismatches");
    CHECK(bad == 0);
}

} // namespace

TEST_CASE("CreditCopper vectors") {
    each_record("CharacterZoneClient_CreditCopper.jsonl", [](const Json& in, const Json& out) -> std::string {
        hailstone::net::Coins c{in["platinum"].u32(), in["gold"].u32(), in["silver"].u32(), in["copper"].u32()};
        hailstone::game::credit_copper(c, in["credit"].i64());
        if (c.platinum != out["platinum"].u32() || c.gold != out["gold"].u32() || c.silver != out["silver"].u32() ||
            c.copper != out["copper"].u32())
            return "coins differ";
        return "";
    });
}

TEST_CASE("ItemBase_CalcItemLinkHash vectors") {
    each_record("ItemBase_CalcItemLinkHash.jsonl", [](const Json& in, const Json& out) -> std::string {
        hailstone::game::ItemLinkHashInput h;
        h.name = in["name"].text;
        h.item_number = in["item_number"].i32();
        h.type = static_cast<std::uint8_t>(in["type"].i64());
        h.cost = in["cost"].i32();
        h.icon = in["icon"].i32();
        h.light = static_cast<std::uint8_t>(in["light"].i64());
        h.item_class = static_cast<std::uint8_t>(in["item_class"].i64());
        h.charm_file_id = in["charm_file_id"].i32();
        h.slots = static_cast<std::uint8_t>(in["slots"].i64());
        h.weight_reduction = static_cast<std::uint8_t>(in["weight_reduction"].i64());
        h.book_lang = static_cast<std::int8_t>(in["book_lang"].i64());
        h.favor = in["favor"].i32();
        h.guild_favor = in["guild_favor"].i32();
        for (std::size_t k = 0; k < 6; ++k) {
            const Json& s = in["sockets"][k];
            if (s.is_null()) continue;
            h.sockets[k] = hailstone::game::ItemLinkHashInput::Augment{s["item_number"].i32(), s["charm_file_id"].i32()};
        }
        h.weight = in["weight"].i32();
        h.contents_value = in["contents_value"].i32();
        h.required_level = in["required_level"].i32();
        h.size = in["size"].i32();
        h.ac = in["ac"].i32();
        h.hp = in["hp"].i32();
        h.mana = in["mana"].i32();
        std::string text = hailstone::game::item_link_hash_text(h);
        if (text != out["text"].text) return "text \"" + text + "\" != \"" + out["text"].text + "\"";
        if (hailstone::game::item_link_hash(h) != out["hash"].u32()) return "hash differs";
        return "";
    });
}

TEST_CASE("make_item_link carries the item's augments and hash") {
    hailstone::net::Item item;
    item.id = 5019;
    item.name = "Fine Steel Long Sword";
    item.value = 1500;
    item.weight = 110;
    hailstone::net::BagItem aug;
    aug.index = 2;
    aug.item.id = 41000;
    item.contents.push_back(aug);
    std::string link = hailstone::game::make_item_link(item, true);
    auto parsed = hailstone::game::parse_item_link(std::string_view(link).substr(1, link.size() - 2));
    REQUIRE(parsed);
    CHECK(parsed->item_id == 5019u);
    CHECK(parsed->augments[2] == 41000u);
    CHECK(parsed->text == item.name);
    CHECK(parsed->hash == hailstone::game::item_link_hash(hailstone::game::item_link_hash_input(item)));
    CHECK(hailstone::game::item_link_hash_text(hailstone::game::item_link_hash_input(item)).rfind("5019Fine Steel Long Sword41000", 0) == 0);
}

namespace {
std::vector<std::uint8_t> from_hex(const std::string& h) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    return out;
}
} // namespace

TEST_CASE("MemChecker0 vectors") {
    each_record("MemChecker0.jsonl", [](const Json& in, const Json& out) -> std::string {
        auto data = from_hex(in["data_hex"].text);
        if (hailstone::game::mem_checker0(data) != out["crc"].u32()) return "crc differs";
        return "";
    });
}

TEST_CASE("EQ_Spell_CalcCRC vectors") {
    each_record("EQ_Spell_CalcCRC.jsonl", [](const Json& in, const Json& out) -> std::string {
        auto want = from_hex(in["record_hex"].text);
        hailstone::game::SpellRecord r = hailstone::game::spell_record_from_line(in["line"].text);
        for (std::size_t k = 0; k < want.size(); ++k)
            if (r[k] != want[k]) {
                char why[96];
                std::snprintf(why, sizeof why, "spell %d: byte +0x%zx is %02x, original %02x", static_cast<int>(in["id"].i64()),
                              k, r[k], want[k]);
                return why;
            }
        if (hailstone::game::mem_checker0(std::span<const std::uint8_t>(r.data(), hailstone::game::kSpellCrcBytes)) !=
            out["crc32_raw"].u32())
            return "raw crc differs";
        if (hailstone::game::spell_record_crc(r) != out["crc"].u32()) return "crc differs";
        return "";
    });
}

TEST_CASE("SpellTable CRCs of the game's spells_us.txt match the original's") {
    const hailstone::GameDir* game = hailstone::test::game_dir();
    if (!game) return;
    hailstone::game::SpellTable table;
    if (!table.load(*game)) return;
    CHECK(table.file_crc() != 0);
    CHECK(table.spell_crc(hailstone::game::kNoSpell) ==
          hailstone::game::spell_record_crc(hailstone::game::unknown_spell_record()));
    // The vectors' lines come from the recording's copy of spells_us.txt; spells whose line is the same here must
    // get the same CRC from the table.
    auto data = game->read("spells_us.txt");
    REQUIRE(data);
    std::string text(reinterpret_cast<const char*>(data->data()), data->size());
    std::map<std::uint32_t, std::string> lines;
    for (std::size_t pos = 0; pos < text.size();) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines[static_cast<std::uint32_t>(std::atoi(line.c_str()))] = line;
        pos = end + 1;
    }
    int same = 0;
    each_record("EQ_Spell_CalcCRC.jsonl", [&](const Json& in, const Json& out) -> std::string {
        auto id = static_cast<std::uint32_t>(in["id"].i64());
        std::string line = in["line"].text;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        auto it = lines.find(id);
        if (it == lines.end() || it->second != line) return "";
        ++same;
        if (table.spell_crc(id) != out["crc"].u32()) return "table crc differs";
        return "";
    });
    MESSAGE(same << " spells with the same line in this spells_us.txt");
}

TEST_CASE("GetSpellDescriptionText vectors") {
    each_record("GetSpellDescriptionText.jsonl", [](const Json& in, const Json& out) -> std::string {
        hailstone::game::SpellDescriptionContext c;
        for (std::size_t k = 0; k < 12; ++k) {
            const Json& s = in["slots"][k];
            c.slots[k] = {s["spa"].i32(), s["base"].i32(), s["base2"].i32(), s["max"].i32(), s["calc"].i32()};
        }
        c.duration_cap = in["duration_cap"].i32();
        c.player_level = in["player_level"].i32();
        c.player_class = static_cast<std::uint8_t>(in["player_class"].i64());
        // The callees answer from the recorded calls, which must come in the same order with the same arguments.
        const Json& calls = in["calls"];
        std::size_t next = 0;
        std::string why;
        auto take = [&](const char* fn) -> const Json* {
            const Json& call = calls[next];
            if (call.is_null() || call["fn"].text != fn) {
                if (why.empty()) why = std::string("call ") + std::to_string(next) + ": expected " + fn;
                return nullptr;
            }
            ++next;
            return &call;
        };
        if (const Json* db = take("CDBStr::GetString")) {
            std::int32_t want = in["id"].i32() > 0 ? in["id"].i32() : in["description_index"].i32();
            if ((*db)["id"].i32() != want || (*db)["kind"].i32() != 6) why = "dbstr id differs";
        }
        c.level_needed = [&](std::uint32_t cls) -> std::uint8_t {
            const Json* call = take("GetSpellLevelNeeded");
            if (!call) return 0;
            if ((*call)["class"].u32() != cls && why.empty()) why = "GetSpellLevelNeeded class differs";
            return static_cast<std::uint8_t>((*call)["ret"].i64());
        };
        c.calc_affect_change = [&](std::int32_t level, std::uint32_t slot) -> std::int32_t {
            const Json* call = take("CalcAffectChange");
            if (!call) return 0;
            if (((*call)["level"].i32() != level || (*call)["slot"].u32() != slot) && why.empty())
                why = "CalcAffectChange arguments differ";
            return (*call)["ret"].i32();
        };
        c.spell_duration = [&](std::int32_t level) -> std::int32_t {
            const Json* call = take("SpellDuration");
            if (!call) return 0;
            if ((*call)["level"].i32() != level && why.empty()) why = "SpellDuration level differs";
            return (*call)["ret"].i32();
        };
        std::string text = hailstone::game::format_spell_description(in["text"].text, c);
        if (!why.empty()) return why;
        if (next != calls.items.size()) return "fewer calls than the original";
        if (text != out["text"].text) return "text \"" + text + "\" != \"" + out["text"].text + "\"";
        return "";
    });
}
