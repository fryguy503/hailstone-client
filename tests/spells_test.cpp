#include <doctest/doctest.h>

#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/spell_ui.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "game/mechanics/character.h"
#include "game/spell_gem.h"
#include "game/spells.h"
#include "game_files.h"
#include "net/opcodes.h"
#include "net/packet_io.h"
#include "net/spell_packets.h"
#include "ui/spell_windows.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

void cstr(ByteWriter& w, std::string_view s) {
    w.bytes({reinterpret_cast<const std::byte*>(s.data()), s.size()});
    w.le<std::uint8_t>(0);
}

AppPacket packet(Op op, ByteWriter& w) { return AppPacket(to_wire(op), w.take()); }

std::vector<std::byte> bytes_of(ByteWriter& w) { return w.take(); }

// A spells_us.txt line with every column "0" except the given ones.
std::string spell_line(std::initializer_list<std::pair<int, std::string>> set) {
    std::vector<std::string> cols(237, "0");
    for (int i = 1; i <= 8; ++i) cols[static_cast<std::size_t>(i)].clear();
    for (const auto& [c, v] : set) cols[static_cast<std::size_t>(c)] = v;
    std::string line;
    for (std::size_t i = 0; i < cols.size(); ++i) line += (i ? "^" : "") + cols[i];
    return line;
}

} // namespace

// ---------------------------------------------------------------- packets

TEST_CASE("OP_CastSpell is the 44-byte CastSpell_Struct the original sends") {
    CastSpell c;
    c.slot = 3;
    c.spell_id = 200;
    c.target_id = 0x1234;
    AppPacket p = encode(c);
    CHECK(p.opcode == 0x1287);
    ByteWriter w;
    w.le<std::uint32_t>(3);
    w.le<std::uint32_t>(200);
    // InventorySlot: Type, Unknown02, Slot, SubIndex, AugIndex = -1 (no item), Unknown01 = 0.
    for (int i = 0; i < 5; ++i) w.le<std::int16_t>(-1);
    w.le<std::int16_t>(0);
    w.le<std::uint32_t>(0x1234);
    w.le<std::uint32_t>(0); // spell CRC (cs_unknown[0])
    w.le<std::uint32_t>(0); // free-target flag + padding (cs_unknown[1])
    w.f32(0);
    w.f32(0);
    w.f32(0);
    CHECK(p.data == bytes_of(w));
    REQUIRE(p.data.size() == kCastSpellSize);
    auto d = decode_cast_spell(p);
    REQUIRE(d);
    CHECK(d->slot == 3);
    CHECK(d->spell_id == 200);
    CHECK(d->target_id == 0x1234);
    CHECK(d->inventory_slot.slot == -1);
}

TEST_CASE("OP_MemorizeSpell carries slot, spell, scribing and reduction") {
    MemorizeSpell m;
    m.slot = 2;
    m.spell_id = 202;
    m.scribing = kMemMemorize;
    AppPacket p = encode(m);
    CHECK(p.opcode == 0x217c);
    ByteWriter w;
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(202);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(0);
    CHECK(p.data == bytes_of(w));
    // Server: spell bar refresh after a cast with a 500 ms reduction.
    ByteWriter s;
    s.le<std::uint32_t>(0);
    s.le<std::uint32_t>(200);
    s.le<std::uint32_t>(3);
    s.le<std::uint32_t>(500);
    auto d = decode_memorize_spell(packet(Op::MemorizeSpell, s));
    REQUIRE(d);
    CHECK(d->scribing == kMemSpellBar);
    CHECK(d->reduction == 500);
}

TEST_CASE("OP_BeginCast: u32 spell, u16 caster, u32 cast time") {
    ByteWriter w;
    w.le<std::uint32_t>(200);
    w.le<std::uint16_t>(77);
    w.le<std::uint32_t>(1500);
    auto b = decode_begin_cast(packet(Op::BeginCast, w));
    REQUIRE(b);
    CHECK(b->spell_id == 200);
    CHECK(b->caster_id == 77);
    CHECK(b->cast_time == 1500);
    CHECK(encode(*b).data.size() == 10);
}

TEST_CASE("OP_InterruptCast: own (no arguments) and others' (the caster's name)") {
    ByteWriter a;
    a.le<std::uint32_t>(5);
    a.le<std::uint32_t>(173); // SPELL_FIZZLE ... as Mob::InterruptSpell sends it to the caster
    auto own = decode_interrupt_cast(packet(Op::InterruptCast, a));
    REQUIRE(own);
    CHECK(own->spawn_id == 5);
    CHECK(own->message_id == 173);
    CHECK(own->args.empty());
    ByteWriter b;
    b.le<std::uint32_t>(9);
    b.le<std::uint32_t>(12205);
    cstr(b, "Bob");
    auto other = decode_interrupt_cast(packet(Op::InterruptCast, b));
    REQUIRE(other);
    REQUIRE(other->args.size() == 1);
    CHECK(other->args[0] == "Bob");
}

TEST_CASE("An empty OP_ManaChange asks the server to interrupt") {
    AppPacket p = encode_stop_cast_request();
    CHECK(p.opcode == 0x5467);
    CHECK(p.data.empty());
}

TEST_CASE("OP_Buff is the 100-byte SpellBuffPacket_Struct") {
    ByteWriter w;
    w.le<std::uint32_t>(42);   // entity
    w.le<std::uint8_t>(2);     // effect type
    w.le<std::uint8_t>(5);     // level
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(0);
    w.f32(1.0f);               // bard modifier
    w.le<std::uint32_t>(202);  // spell
    w.le<std::uint32_t>(270);  // duration (tics)
    w.le<std::uint32_t>(42);   // caster
    w.le<std::uint32_t>(0);    // num hits
    w.f32(1.0f);
    w.f32(2.0f);
    w.f32(3.0f);
    w.le<std::uint32_t>(0);
    for (int i = 0; i < 12; ++i) w.le<std::int32_t>(0);
    w.le<std::uint32_t>(3);    // slot
    w.le<std::uint32_t>(2);    // fade: 2 = refresh (ENCODE(OP_Buff))
    AppPacket p = packet(Op::Buff, w);
    REQUIRE(p.data.size() == kBuffPacketSize);
    auto b = decode_buff(p);
    REQUIRE(b);
    CHECK(b->entity_id == 42);
    CHECK(b->buff.spell_id == 202);
    CHECK(b->buff.duration == 270);
    CHECK(b->buff.level == 5);
    CHECK(b->slot == 3);
    CHECK(b->fade == 2);
    CHECK(encode(*b).data == p.data);
}

TEST_CASE("OP_BuffCreate: the full list and ENCODE(OP_Buff)'s 29-byte removal") {
    ByteWriter w;
    w.le<std::uint32_t>(42);
    w.le<std::uint32_t>(0);   // tic timer
    w.le<std::uint8_t>(1);    // all buffs
    w.le<std::uint16_t>(2);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(202);
    w.le<std::uint32_t>(270);
    w.le<std::uint32_t>(0);
    cstr(w, "Nmscspelltest");
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(11);
    w.le<std::uint32_t>(90);
    w.le<std::uint32_t>(0);
    cstr(w, "");
    w.le<std::uint8_t>(0); // type: own window
    AppPacket p = packet(Op::BuffCreate, w);
    auto b = decode_buff_icons(p);
    REQUIRE(b);
    CHECK(b->entity_id == 42);
    CHECK(b->all_buffs == 1);
    REQUIRE(b->entries.size() == 2);
    CHECK(b->entries[0].spell_id == 202);
    CHECK(b->entries[0].tics == 270);
    CHECK(b->entries[0].caster == "Nmscspelltest");
    CHECK(b->entries[1].slot == 1);
    CHECK(encode(*b, false).data == p.data);

    ByteWriter r; // rof2.cpp ENCODE(OP_Buff), bufffade 1
    r.le<std::uint32_t>(42);
    r.le<std::uint32_t>(0);
    r.le<std::uint8_t>(0);
    r.le<std::uint16_t>(1);
    r.le<std::uint32_t>(3);
    r.le<std::uint32_t>(0xffffffff);
    r.le<std::uint32_t>(0);
    r.le<std::uint32_t>(0);
    r.le<std::uint8_t>(0);
    r.le<std::uint8_t>(0);
    AppPacket rp = packet(Op::BuffCreate, r);
    CHECK(rp.data.size() == 29);
    auto rm = decode_buff_icons(rp);
    REQUIRE(rm);
    REQUIRE(rm->entries.size() == 1);
    CHECK(rm->entries[0].slot == 3);
    CHECK(rm->entries[0].spell_id == 0xffffffff);
    CHECK(encode(*rm, true).opcode == to_wire(Op::TargetBuffs));
}

TEST_CASE("OP_Action is the 56-byte ActionAlt_Struct") {
    ByteWriter w;
    w.le<std::uint16_t>(10);  // target
    w.le<std::uint16_t>(42);  // source
    w.le<std::uint16_t>(5);   // level
    w.le<std::uint32_t>(0);
    w.f32(1.0f);              // instrument mod
    w.f32(0.0f);              // force
    w.f32(128.0f);            // heading
    w.f32(0.0f);              // pitch
    w.le<std::uint8_t>(231);  // spell
    w.le<std::uint32_t>(0);   // damage
    w.le<std::uint16_t>(0);
    w.le<std::uint32_t>(200); // spell id
    w.le<std::uint8_t>(5);
    w.le<std::uint8_t>(4);    // landed
    w.le<std::uint8_t>(0);
    for (int i = 0; i < 6; ++i) w.le<std::int16_t>(i == 1 || i == 5 ? 0 : -1);
    w.le<std::uint32_t>(0);
    AppPacket p = packet(Op::Action, w);
    REQUIRE(p.data.size() == kActionSize);
    auto a = decode_action(p);
    REQUIRE(a);
    CHECK(a->target == 10);
    CHECK(a->source == 42);
    CHECK(a->type == 231);
    CHECK(a->spell_id == 200);
    CHECK(a->effect_flag == 4);
    CHECK(a->hit_heading == doctest::Approx(128.0f));
    CHECK(encode(*a).data == p.data);
}

TEST_CASE("Small spell packets: DeleteSpell, ColoredText, Stamina, LoadSpellSet, BuffRemoveRequest") {
    DeleteSpell d;
    d.book_slot = 17;
    AppPacket dp = encode(d);
    ByteWriter dw;
    dw.le<std::int16_t>(17);
    dw.zeros(6);
    CHECK(dp.data == bytes_of(dw));
    ByteWriter echo;
    echo.le<std::int16_t>(17);
    echo.zeros(2);
    echo.le<std::uint8_t>(1);
    echo.zeros(3);
    auto de = decode_delete_spell(packet(Op::DeleteSpell, echo));
    REQUIRE(de);
    CHECK(de->book_slot == 17);
    CHECK(de->success == 1);

    // ColoredText as EQEmu sends a fade: struct (4 + 4) + strlen, no terminator of its own.
    ByteWriter ct;
    ct.le<std::uint32_t>(15);
    std::string fade = "You feel weaker.";
    ct.bytes({reinterpret_cast<const std::byte*>(fade.data()), fade.size()});
    ct.zeros(4);
    auto c = decode_colored_text(packet(Op::ColoredText, ct));
    REQUIRE(c);
    CHECK(c->color == 15);
    CHECK(c->text == fade);

    ByteWriter st;
    st.le<std::uint32_t>(6000);
    st.le<std::uint32_t>(5000);
    auto s = decode_stamina(packet(Op::Stamina, st));
    REQUIRE(s);
    CHECK(s->food == 6000);
    CHECK(s->water == 5000);

    AppPacket ls = encode_load_spell_set({0, 3});
    REQUIRE(ls.data.size() == 16);
    CHECK(std::to_integer<int>(ls.data[0]) == 1);
    CHECK(std::to_integer<int>(ls.data[1]) == 0);
    CHECK(std::to_integer<int>(ls.data[3]) == 1);

    AppPacket br = encode_buff_remove_request(4, 42);
    ByteWriter bw;
    bw.le<std::uint32_t>(4);
    bw.le<std::uint32_t>(42);
    CHECK(br.data == bytes_of(bw));
    CHECK(br.opcode == to_wire(Op::BuffRemoveRequest));
}

// ---------------------------------------------------------------- rules

TEST_CASE("spells_us.txt lines: the columns of EQ_Spell_InitFromLine") {
    auto line = spell_line({{0, "200"}, {1, "Minor Healing"}, {6, "You feel a little better."}, {7, " feels a little better."},
                            {9, "100"}, {13, "1500"}, {14, "2250"}, {15, "3000"}, {19, "10"}, {20, "10"}, {44, "20"},
                            {83, "1"}, {86, "0"}, {87, "254"}, {98, "5"}, {105, "1"}, {104, "255"}, {144, "99"},
                            {155, "200"}, {166, "7"}, {184, "1"}, {231, "2.5"}});
    auto s = game::SpellTable::parse_line(line);
    REQUIRE(s);
    CHECK(s->id == 200);
    CHECK(s->name == "Minor Healing");
    CHECK(s->cast_on_you == "You feel a little better.");
    CHECK(s->cast_on_other == " feels a little better.");
    CHECK(s->range == doctest::Approx(100.0f));
    CHECK(s->cast_time == 1500);
    CHECK(s->recovery_time == 2250);
    CHECK(s->recast_time == 3000);
    CHECK(s->mana == 10);
    CHECK(s->base[0] == 10);
    CHECK(s->max[0] == 20);
    CHECK(s->beneficial());
    CHECK(s->effect[1] == 254);
    CHECK(s->target_type == 5);
    CHECK(s->level_needed(2) == 1);
    CHECK(s->level_needed(1) == 255);
    CHECK(s->spell_icon == 99);
    CHECK(s->description_id == 200);
    CHECK(s->endurance_cost == 7);
    CHECK(s->cast_not_standing);
    CHECK(s->min_range == doctest::Approx(2.5f));
    CHECK_FALSE(game::SpellTable::parse_line("# comment"));
    CHECK_FALSE(game::SpellTable::parse_line(""));

    game::SpellTable t;
    std::string text = line + "\r\n" + spell_line({{0, "45001"}, {1, "Too far"}}) + "\n" + spell_line({{0, "7"}, {1, "Never"}});
    CHECK(t.parse(text) == 1); // LoadTextSpells stops at an id above 45000
    CHECK(t.get(200));
    CHECK_FALSE(t.get(7));
}

TEST_CASE("Cast time: 100 ms or less is instant") {
    CHECK(game::is_instant(100));
    CHECK_FALSE(game::is_instant(101));
}

TEST_CASE("Target types, multiclass levels, durations, icons") {
    CHECK(game::target_type_needs_target(5));
    CHECK_FALSE(game::target_type_needs_target(6));
    CHECK(game::target_type_requires_target(8));
    CHECK_FALSE(game::target_type_requires_target(14)); // pet: retargeted to the pet
    CHECK_FALSE(game::target_type_requires_target(41));
    game::SpellData s;
    s.class_level.fill(255);
    s.class_level[1] = 1;  // cleric
    s.class_level[11] = 4; // wizard
    CHECK(game::level_needed(s, 0, 12) == 4);
    CHECK(game::level_needed(s, (1u << 1) | (1u << 11), 16) == 1);
    CHECK(game::level_needed(s, 1u << 0, 2) == 255);
    CHECK(game::format_duration(12000) == "12s");
    CHECK(game::format_duration(299000) == "4:59");
    CHECK(game::format_duration(3900000) == "1:05:00");
    CHECK(game::format_duration(4806735ull * 6000) == "333d");
    CHECK(game::buff_ms_left(10, 1000, 7000) == 54000);
    CHECK(game::buff_ms_left(1, 0, 60000) == 0);
    CHECK(game::buff_tics_left(10, 1000, 6999) == 10); // not a whole tick yet
    CHECK(game::buff_tics_left(10, 1000, 7000) == 9);
    CHECK(game::buff_tics_left(10, 1000, 19000) == 7);
    CHECK(game::buff_tics_left(2, 0, 60000) == 0);         // run out: 0, not wrapped
    CHECK(game::buff_tics_left(10, 5000, 1000) == 10);     // a clock behind the packet counts nothing
    CHECK(game::buff_tics_left(-1, 0, 600000) == -1); // permanent: negative as sent, never counted down
    CHECK(game::buff_tics_left(-2, 0, 600000) == -2);
    CHECK(game::buff_tics_left(-4, 0, 600000) == -4);       // EQEmu's aura (formula 51)
    CHECK(game::buff_tics_left(-1000, 0, 600000) == -1000); // EQEmu's PERMANENT_BUFF_DURATION
    CHECK(game::buff_tics_left(0x7fffffff, 0, 12000) == 0x7ffffffd); // the largest count runs down too
    CHECK(game::buff_is_permanent(-2, nullptr));
    CHECK(game::buff_is_permanent(-4, nullptr));    // EQEmu's aura (formula 51)
    CHECK(game::buff_is_permanent(-1000, nullptr)); // EQEmu's PERMANENT_BUFF_DURATION
    CHECK_FALSE(game::buff_is_permanent(0x7fffffff, nullptr));
    CHECK(game::buff_ms_left(-1, 0, 0) == 0); // a negative count: permanent, no time to count
    auto c = ui::spell_icon_cell(99);
    REQUIRE(c);
    CHECK(c->sheet == 3);  // 99 / 36 = 2 → spells03
    CHECK(c->x == 120);    // cell 27: column 3
    CHECK(c->y == 160);    // row 4
    CHECK_FALSE(ui::spell_icon_cell(300));
}

TEST_CASE("SpellTable::by_group_and_rank (SpellManager::GetSpellByGroupAndRank)") {
    auto line = [](int id, int group, int rank, int subgroup) {
        std::vector<std::string> f(237, "0");
        f[0] = std::to_string(id);
        f[1] = "Spell" + std::to_string(id);
        for (int k = 0; k < 12; ++k) {
            f[86 + static_cast<std::size_t>(k)] = "254";
            f[70 + static_cast<std::size_t>(k)] = "100";
        }
        f[207] = std::to_string(group);
        f[208] = std::to_string(rank);
        f[236] = std::to_string(subgroup);
        std::string s;
        for (std::size_t i = 0; i < f.size(); ++i) s += (i ? "^" : "") + f[i];
        return s + "\n";
    };
    game::SpellTable t;
    REQUIRE(t.parse(line(10, 5, 1, 2) + line(11, 5, 2, 2) + line(12, 5, 3, 2) + line(13, 5, 2, 9) + line(14, 6, 1, 2) +
                    line(15, 0, 1, 2)) == 6);
    CHECK(t.by_group_and_rank(5, 2, 2, true)->id == 11);   // exactly that rank
    CHECK(t.by_group_and_rank(5, 2, 2, false)->id == 11);
    CHECK(t.by_group_and_rank(5, 2, 5, true)->id == 12);   // the highest below
    CHECK_FALSE(t.by_group_and_rank(5, 2, 5, false));      // no lower ranks asked
    CHECK(t.by_group_and_rank(5, 2, -1, false)->id == 12); // -1: the highest
    CHECK_FALSE(t.by_group_and_rank(5, 2, 0, true));
    CHECK(t.by_group_and_rank(5, 9, 3, true)->id == 13);   // the subgroup must match
    CHECK_FALSE(t.by_group_and_rank(0, 2, 1, true));       // group 0 is not hashed
    CHECK_FALSE(t.by_group_and_rank(7, 2, 1, true));
}

TEST_CASE("SpellTable: the indexed lookups match a scan of the whole table") {
    // The scans find_by_name and by_group_and_rank made before their indices, kept here as the reference.
    struct Row {
        std::uint32_t id;
        std::string name;
        int group, rank, subgroup;
    };
    std::vector<Row> rows;
    std::uint32_t seed = 12345;
    auto next = [&seed](std::uint32_t mod) {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) % mod;
    };
    const char* names[] = {"Clarity", "CLARITY", "clarity", "Spirit of Wolf", "Gate", "", "gate"};
    std::string text;
    for (std::uint32_t id = 1; id < 400; ++id) {
        if (next(4) == 0) continue;
        Row r{id, names[next(7)], static_cast<int>(next(4)), static_cast<int>(next(6)) - 1,
              static_cast<int>(next(3))};
        std::vector<std::string> f(237, "0");
        f[0] = std::to_string(r.id);
        f[1] = r.name;
        for (int k = 0; k < 12; ++k) {
            f[86 + static_cast<std::size_t>(k)] = "254";
            f[70 + static_cast<std::size_t>(k)] = "100";
        }
        f[207] = std::to_string(r.group);
        f[208] = std::to_string(r.rank);
        f[236] = std::to_string(r.subgroup);
        for (std::size_t i = 0; i < f.size(); ++i) text += (i ? "^" : "") + f[i];
        text += "\n";
        rows.push_back(r);
    }
    game::SpellTable t;
    REQUIRE(t.parse(text) == rows.size());
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    auto scan_name = [&](const std::string& name) -> long {
        long best = -1;
        for (const auto& r : rows)
            if (lower(r.name) == lower(name) && (best < 0 || r.id < static_cast<std::uint32_t>(best))) best = r.id;
        return best;
    };
    auto scan_rank = [&](int group, int subgroup, int rank, bool below) -> long {
        if (group <= 0) return -1;
        const Row* exact = nullptr;
        const Row* best = nullptr;
        for (const auto& r : rows) {
            if (r.group != group || r.subgroup != subgroup) continue;
            if (r.rank == rank) {
                if (!exact || exact->id < r.id) exact = &r;
                continue;
            }
            if (((below && r.rank < rank) || rank == -1) &&
                (!best || best->rank < r.rank || (best->rank == r.rank && best->id < r.id)))
                best = &r;
        }
        const Row* got = exact ? exact : best;
        return got ? static_cast<long>(got->id) : -1;
    };
    for (const char* n : {"clarity", "Clarity", "GATE", "", "Spirit of wolf", "Nothing"}) {
        const game::SpellData* s = t.find_by_name(n);
        CHECK((s ? static_cast<long>(s->id) : -1) == scan_name(n));
    }
    for (int group = -1; group <= 4; ++group)
        for (int subgroup = 0; subgroup <= 3; ++subgroup)
            for (int rank = -2; rank <= 6; ++rank)
                for (bool below : {false, true}) {
                    const game::SpellData* s = t.by_group_and_rank(group, subgroup, rank, below);
                    CHECK((s ? static_cast<long>(s->id) : -1) == scan_rank(group, subgroup, rank, below));
                }
}

TEST_CASE("Spell hit lines (ReportSuccessfulHit, type 231)") {
    game::EqStrings strings;
    strings.set(4600, "%1 You have been healed for %2 points.");
    strings.set(12203, "%1  You have taken %2 points of damage.");
    game::WorldState world;
    world.player_id = 1;
    world.spawns[1].name = "Me";
    world.spawns[2].name = "a_rat01";
    game::SpellData s;
    s.cast_on_you = "You feel better.";
    s.cast_on_other = " feels better.";
    CHECK(game::spell_hit_message(strings, world, s, 1, 0) == "You feel better.");
    CHECK(game::spell_hit_message(strings, world, s, 1, -15) == "You feel better. You have been healed for 15 points.");
    CHECK(game::spell_hit_message(strings, world, s, 1, 7) == "You feel better.  You have taken 7 points of damage.");
    CHECK(game::spell_hit_message(strings, world, s, 2, 0) == "a rat feels better.");
    s.cast_on_other.clear();
    CHECK_FALSE(game::spell_hit_message(strings, world, s, 2, 0));
    CHECK_FALSE(game::spell_hit_message(strings, world, s, 99, 0));
}

TEST_CASE("Spell hit modes use message identity and numeric substitutions") {
    using Report = game::SpellHitReport;
    struct Fixture {
        std::uint32_t id;
        std::vector<std::string> args;
        Report kind;
        std::string compact, amount;
    };
    const std::array fixtures{
        Fixture{9073, {"Caster", "Victim", "1,234"}, Report::DirectDamage,
                "Caster -> Victim: 1,234 damage", "1,234"},
        Fixture{434, {"Victim", "27"}, Report::DirectDamage, "Victim: 27 non-melee", "27"},
        Fixture{12481, {"28"}, Report::DirectDamage, "You: 28 non-melee", "28"},
        Fixture{428, {"29"}, Report::DirectDamage, "You: 29 critical blast", "29"},
        Fixture{1040, {"Caster", "30"}, Report::DirectDamage, "Caster: 30 critical blast", "30"},
        Fixture{9072, {"Victim", "31", "Venom"}, Report::DamageOverTime, "Victim: 31 DoT (Venom)", "31"},
        Fixture{12954, {"32", "Venom", "Caster"}, Report::DamageOverTime, "You: 32 DoT", "32"},
        Fixture{13327, {"Victim", "33", "Venom", "Caster"}, Report::DamageOverTime, "Victim: 33 DoT", "33"},
        Fixture{9068, {"Victim", "34"}, Report::Healing, "You -> Victim: 34 heal", "34"},
        Fixture{419, {"Caster", "35"}, Report::Healing, "Caster -> You: 35 heal", "35"},
        Fixture{427, {"36"}, Report::Healing, "You: 36 critical heal", "36"},
        Fixture{1039, {"Caster", "37"}, Report::Healing, "Caster: 37 critical heal", "37"},
        Fixture{12976, {"38", "Renewal"}, Report::HealingOverTime, "You: 38 HoT (Renewal)", "38"},
        Fixture{12997, {"Victim", "39", "Renewal"}, Report::HealingOverTime,
                "You -> Victim: 39 HoT (Renewal)", "39"},
        Fixture{12998, {"Caster", "40", "Renewal"}, Report::HealingOverTime,
                "Caster -> You: 40 HoT (Renewal)", "40"},
    };
    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.id);
        const auto line = game::spell_message_hit_report(fixture.id, fixture.args);
        REQUIRE(line);
        CHECK(line->kind == fixture.kind);
        CHECK(game::report_spell_hit_line(*line, "Complete localized sentence", 0) == "Complete localized sentence");
        CHECK(game::report_spell_hit_line(*line, "Complete localized sentence", 1) == fixture.compact);
        CHECK(game::report_spell_hit_line(*line, "Complete localized sentence", 2) == fixture.amount);
        CHECK(game::report_spell_hit_line(*line, "Complete localized sentence", -1) == "Complete localized sentence");
        CHECK(game::report_spell_hit_line(*line, "Complete localized sentence", 3) == "Complete localized sentence");
        for (std::size_t n = 0; n < fixture.args.size(); ++n)
            CHECK_FALSE(game::spell_message_hit_report(fixture.id, std::span(fixture.args).first(n)));
    }
    // DoT caster/spell arguments have been emitted in either order by servers; none is mislabelled in compact
    // incoming/observer lines. A name containing a number is not mistaken for the damage substitution.
    CHECK(game::spell_message_hit_report(12954, std::array<std::string, 3>{"32", "Caster42", "Venom"})->abbreviated ==
          "You: 32 DoT");
    CHECK(game::spell_message_hit_report(13327, std::array<std::string, 4>{"Victim42", "33", "Caster", "Venom"})->abbreviated ==
          "Victim42: 33 DoT");
}

TEST_CASE("Spell hit modes preserve malformed and unrelated messages") {
    for (const std::string amount : {"", "12 HP", "1.5", "-1", "+1", " 1", "1 ", "12,34", ",123", "123,",
                                     "1,,234", "1234,567", "18446744073709551616"}) {
        CAPTURE(amount);
        CHECK_FALSE(game::spell_message_hit_report(9073, std::array<std::string, 3>{"Caster", "Victim", amount}));
    }
    for (const std::string amount : {"0", "1", "1234", "1,234", "12,345,678", "18446744073709551615"}) {
        CAPTURE(amount);
        const auto line = game::spell_message_hit_report(9073, std::array<std::string, 3>{"Caster", "Victim", amount});
        REQUIRE(line);
        CHECK(line->amount == amount);
    }
    CHECK_FALSE(game::spell_message_hit_report(9073, std::array<std::string, 3>{"", "Victim", "123"}));
    CHECK_FALSE(game::spell_message_hit_report(9073, std::array<std::string, 3>{"Caster", "", "123"}));
    // A casting line, a resist line, a buff message and arbitrary custom ids never become numbers-only.
    for (const std::uint32_t id : {12205u, 12207u, 12003u, 99999u})
        CHECK_FALSE(game::spell_message_hit_report(id, std::array<std::string, 3>{"Caster", "Victim", "123"}));
}

TEST_CASE("Spell hit modes recognize complete combat text on matching message types") {
    using Report = game::SpellHitReport;
    struct Fixture {
        std::string text;
        std::uint32_t color;
        Report kind;
        std::string compact, amount;
    };
    const std::array fixtures{
        Fixture{"Diddles hit a Broken Skull guardian for 486 points of non-melee damage. (Time Rend)", 379,
                Report::DirectDamage, "Diddles -> a Broken Skull guardian: 486 damage (Time Rend)", "486"},
        Fixture{"Diddles has healed herself for 315 points of damage. (Killing Spree)", 375,
                Report::Healing, "Diddles -> herself: 315 heal (Killing Spree)", "315"},
        Fixture{"You have healed your pet for 1 point of damage.", 374,
                Report::Healing, "You -> your pet: 1 heal", "1"},
        Fixture{"Caster42 hit a guardian for 1 point of non-melee damage.", 397,
                Report::DirectDamage, "Caster42 -> a guardian: 1 damage", "1"},
        Fixture{"Caster hit a guardian for 1,234 points of non-melee damage. (Venom)", 376,
                Report::DamageOverTime, "Caster -> a guardian: 1,234 DoT (Venom)", "1,234"},
    };
    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.text);
        const auto line = game::spell_text_hit_report(fixture.text, fixture.color);
        REQUIRE(line);
        CHECK(line->kind == fixture.kind);
        CHECK(line->amount == fixture.amount);
        CHECK(game::report_spell_hit_line(*line, fixture.text, 0) == fixture.text);
        CHECK(game::report_spell_hit_line(*line, fixture.text, 1) == fixture.compact);
        CHECK(game::report_spell_hit_line(*line, fixture.text, 2) == fixture.amount);
    }
    const std::string damage = "Caster hit Victim for 27 points of non-melee damage.";
    const std::string heal = "Caster has healed Victim for 28 points of damage.";
    for (std::uint32_t color : {283u, 302u, 379u, 397u, 398u}) {
        CAPTURE(color);
        const auto line = game::spell_text_hit_report(damage, color);
        REQUIRE(line);
        CHECK(line->kind == Report::DirectDamage);
    }
    for (std::uint32_t color : {376u, 377u, 399u, 400u, 401u}) {
        CAPTURE(color);
        const auto line = game::spell_text_hit_report(damage, color);
        REQUIRE(line);
        CHECK(line->kind == Report::DamageOverTime);
        CHECK_FALSE(game::spell_text_hit_report(heal, color));
    }
    for (std::uint32_t color : {283u, 302u, 374u, 375u, 402u, 403u, 404u}) {
        CAPTURE(color);
        const auto line = game::spell_text_hit_report(heal, color);
        REQUIRE(line);
        // A raw healing sentence supplies no periodic-tick discriminator.
        CHECK(line->kind == Report::Healing);
    }
    for (const std::string amount : {"0", "1,234", "12,345,678", "18446744073709551615"}) {
        const auto line = game::spell_text_hit_report("Caster hit Victim for " + amount +
                                                        " points of non-melee damage.", 283);
        REQUIRE(line);
        CHECK(line->amount == amount);
    }
}

TEST_CASE("Spell hit modes leave malformed, ambiguous and non-combat text intact") {
    const std::string damage = "Caster hit Victim for 27 points of non-melee damage.";
    const std::string heal = "Caster has healed Victim for 28 points of damage.";
    for (std::uint32_t color : {0u, 10u, 256u, 274u, 289u, 336u, 99999u}) {
        CAPTURE(color);
        CHECK_FALSE(game::spell_text_hit_report(damage, color));
        CHECK_FALSE(game::spell_text_hit_report(heal, color));
    }
    for (std::uint32_t color : {374u, 375u, 402u, 403u, 404u})
        CHECK_FALSE(game::spell_text_hit_report(damage, color));
    for (std::uint32_t color : {379u, 397u, 398u})
        CHECK_FALSE(game::spell_text_hit_report(heal, color));
    for (const std::string amount : {"", "12 HP", "1.5", "-1", "+1", " 1", "1 ", "12,34", ",123", "123,",
                                     "1,,234", "1234,567", "18446744073709551616"}) {
        CAPTURE(amount);
        CHECK_FALSE(game::spell_text_hit_report("Caster hit Victim for " + amount +
                                                  " points of non-melee damage.", 283));
        CHECK_FALSE(game::spell_text_hit_report("Caster has healed Victim for " + amount +
                                                  " points of damage.", 375));
    }
    for (const std::string text : {
             "", "Caster casts a spell for 27 points of non-melee damage.",
             " hit Victim for 27 points of non-melee damage.",
             " Caster hit Victim for 27 points of non-melee damage.",
             "Caster hit  for 27 points of non-melee damage.",
             "Caster hit Victim  for 27 points of non-melee damage.",
             "Caster hit Another hit Victim for 27 points of non-melee damage.",
             "Caster has healed Another has healed Victim for 27 points of damage.",
             "Caster has healed Another have healed Victim for 27 points of damage.",
             "Caster has healed Another hit Victim for 27 points of non-melee damage.",
             "Caster hit Another has healed Victim for 27 points of damage.",
             "Caster hit Victim for 27 points of non-melee damage",
             "Caster hit Victim for 27 points of non-melee damage. trailing text",
             "Caster hit Victim for 27 points of non-melee damage.(Venom)",
             "Caster hit Victim for 27 points of non-melee damage. ()",
             "Caster hit Victim for 27 points of non-melee damage. ( )",
             "Caster hit Victim for 27 points of non-melee damage. (Venom",
             "Caster hit Victim for 27 points of non-melee damage. (Ve(nom)",
             "Caster hit Victim for 27 points of non-melee damage. (Ve)nom)",
             "Caster hit Victim for 27 points of non-melee damage. (Venom) extra",
             "Caster\nhit Victim for 27 points of non-melee damage.",
             "Caster hit Vic\ttim for 27 points of non-melee damage.",
             "Caster hit Victim for 27 points of non-melee damage. (Ve\nnom)",
         }) {
        CAPTURE(text);
        CHECK_FALSE(game::spell_text_hit_report(text, 283));
    }
}

TEST_CASE("Spell hit modes recognize stock EQEmu's caster-less heal") {
    // zone/attack.cpp Mob::HealDamage: Message(Chat::NonMelee, "You have been healed for %d points of damage.")
    const std::string text = "You have been healed for 42 points of damage.";
    const auto line = game::spell_text_hit_report(text, 283);
    REQUIRE(line);
    CHECK(line->kind == game::SpellHitReport::Healing);
    CHECK(line->amount == "42");
    CHECK(game::report_spell_hit_line(*line, text, 0) == text);
    CHECK(game::report_spell_hit_line(*line, text, 1) == "You: 42 heal");
    CHECK(game::report_spell_hit_line(*line, text, 2) == "42");
    CHECK_FALSE(game::spell_text_hit_report(text, 376)); // a DoT type carries no heal
    CHECK_FALSE(game::spell_text_hit_report(text, 256));
    CHECK_FALSE(game::spell_text_hit_report("You have been healed for 42 points of non-melee damage.", 283));
    CHECK_FALSE(game::spell_text_hit_report("You have been healed for 4x points of damage.", 283));
}

TEST_CASE("Spell hit modes keep speaker prefixes whole and accept multi-word NPC names") {
    for (const std::string text : {
             "Somebody says, Bob hit Fred for 5 points of non-melee damage.",
             "Somebody says, 'Bob hit Fred for 5 points of non-melee damage.",
             "Bob says Bob hit Fred for 5 points of non-melee damage.",
             "Bob tells you Bob hit Fred for 5 points of non-melee damage.",
             "Bob shouts Bob hit Fred for 5 points of non-melee damage.",
             "Bob auctions Bob hit Fred for 5 points of non-melee damage.",
             "Bob tells General:1 Bob hit Fred for 5 points of non-melee damage.",
             "\"Bob\" hit Fred for 5 points of non-melee damage.",
             "Bob hit Fred, Sue for 5 points of non-melee damage.",
             "Somebody says, Bob has healed Fred for 5 points of damage.",
             "Bob has healed Fred says for 5 points of damage.",
         }) {
        CAPTURE(text);
        CHECK_FALSE(game::spell_text_hit_report(text, 283));
    }
    struct Fixture {
        std::string text, compact;
    };
    for (const auto& fixture : {
             Fixture{"Lord Nagafen hit a gnoll`s pup for 5 points of non-melee damage.",
                     "Lord Nagafen -> a gnoll`s pup: 5 damage"},
             Fixture{"Ka'rul the Wise hit an orc pawn for 5 points of non-melee damage.",
                     "Ka'rul the Wise -> an orc pawn: 5 damage"},
             Fixture{"Guard Gehnus has healed Captain Tillin for 12 points of damage.",
                     "Guard Gehnus -> Captain Tillin: 12 heal"},
         }) {
        CAPTURE(fixture.text);
        const auto line = game::spell_text_hit_report(fixture.text, 283);
        REQUIRE(line);
        CHECK(line->abbreviated == fixture.compact);
    }
}

TEST_CASE("OP_Damage spell hit modes retain nonnumeric landing lines") {
    game::WorldState world;
    world.player_id = 1;
    world.spawns[1].name = "Me";
    world.spawns[2].name = "a_rat01";
    game::SpellData spell;
    spell.name = "Test Spell";
    spell.duration_formula = 1; // an initial hit of a lasting spell is still direct damage
    auto damage = game::spell_damage_hit_report(world, spell, 1, 123);
    REQUIRE(damage);
    CHECK(damage->kind == game::SpellHitReport::DirectDamage);
    CHECK(damage->abbreviated == "You: 123 damage (Test Spell)");
    CHECK(game::report_spell_hit_line(*damage, "Native complete landing line", 2) == "123");
    const auto healing = game::spell_damage_hit_report(world, spell, 1, (-2147483647 - 1));
    REQUIRE(healing);
    CHECK(healing->kind == game::SpellHitReport::Healing);
    CHECK(healing->amount == "2147483648");
    CHECK(healing->abbreviated == "You: 2147483648 heal (Test Spell)");
    CHECK_FALSE(game::spell_damage_hit_report(world, spell, 1, 0));
    CHECK_FALSE(game::spell_damage_hit_report(world, spell, 2, 123));
    CHECK_FALSE(game::spell_damage_hit_report(world, spell, 99, 123));
    world.spawns.erase(1);
    CHECK_FALSE(game::spell_damage_hit_report(world, spell, 1, 123));
}

TEST_CASE("The game's spells_us.txt" * doctest::skip(test::game_dir() == nullptr)) {
    game::SpellTable t;
    REQUIRE(t.load(*test::game_dir()));
    CHECK(t.size() > 40000);
    const auto* heal = t.get(200);
    REQUIRE(heal);
    CHECK(heal->name == "Minor Healing");
    CHECK(heal->cast_on_you == "You feel a little better.");
    CHECK(heal->cast_time == 1500);
    CHECK(heal->mana == 10);
    CHECK(heal->target_type == 5);
    CHECK(heal->level_needed(2) == 1); // cleric
    CHECK(heal->spell_icon == 99);
    CHECK(heal->base[0] == 10);
    CHECK(heal->max[0] == 20);
    game::EqStrings strings;
    strings.load(*test::game_dir());
    // Effect 1 is SPA 0 with formula 2 (base + 2 x level, up to the max): #1 is CalcAffectChange at a level-1
    // cleric's level (12), @1 at level 100 (the max, 20).
    game::mechanics::GameTables tables;
    game::mechanics::CharacterSheet sheet;
    sheet.class_id = 2;
    sheet.level = 1;
    game::mechanics::Character cleric{sheet, tables, [&](std::int32_t id) { return t.get(static_cast<std::uint32_t>(id)); }};
    CHECK(app::spell_description(*heal, strings, {&cleric, 1, 2}) ==
          "Mends minor wounds, healing between 12 and 20 hit points.");
    for (std::uint32_t id = 1; id < 3000; ++id)
        if (const auto* s = t.get(id)) CHECK(s->id == id);
}

TEST_CASE("SPA 57: SpellCache_GetBuffValue(57, k) holds the levitate effects with Base2 k") {
    // CacheBuffSlotEffects keys SPA 57 by Base2: k = 0 is levitation (mode 2), k = 1
    // levitation while running (mode 5); an inverted buff (type 4) subtracts.
    game::SpellData lev, run;
    lev.id = 261;
    run.id = 2517;
    for (auto* s : {&lev, &run}) {
        s->effect.fill(254);
        s->effect[0] = 57;
        s->base[0] = 1;
        s->formula[0] = 100;
        s->max[0] = 0;
        s->spell_type = 1;
        s->duration_formula = 11;
        s->duration_cap = 100;
    }
    run.base2[0] = 1;
    game::mechanics::GameTables tables;
    game::mechanics::CharacterSheet sheet;
    sheet.class_id = 2;
    sheet.level = 20;
    game::mechanics::SheetBuff b;
    b.type = 2;
    b.level = 20;
    b.spell_id = 2517;
    sheet.buffs.push_back(b);
    auto lookup = [&](std::int32_t id) -> const game::SpellData* { return id == 261 ? &lev : id == 2517 ? &run : nullptr; };
    game::mechanics::Character ch{sheet, tables, lookup};
    CHECK(ch.cached_buff(57, 0) == 0);
    CHECK(ch.cached_buff(57, 1) == 1);
    sheet.buffs[0].spell_id = 261;
    game::mechanics::Character ch2{sheet, tables, lookup};
    CHECK(ch2.cached_buff(57, 0) == 1);
    CHECK(ch2.cached_buff(57, 1) == 0);
}

TEST_CASE("A spell's target-type colour (0x4ad890: the gem tint and the memorize menu's rows)") {
    using hailstone::game::spell_target_color;
    CHECK(spell_target_color(5, 0) == 0xffde2121u);  // single target (the default)
    CHECK(spell_target_color(6, 0) == 0xffcede29u);  // self
    CHECK(spell_target_color(41, 0) == 0xff9c31deu); // group
    CHECK(spell_target_color(4, 0) == 0xff5242deu);  // PB AE
    CHECK(spell_target_color(8, 2999) == 0xff42de63u); // targeted AE: the AE duration under 3000 ms switches it
    CHECK(spell_target_color(8, 3000) == 0xffde7b21u);
    CHECK(spell_target_color(45, 0) == 0xff003300u);
    CHECK(spell_target_color(45, 3000) == 0xffcc3300u);
    CHECK(spell_target_color(44, 0) == 0xff00deccu);
    CHECK(spell_target_color(46, 0) == 0xffde33deu);
    CHECK(spell_target_color(34, 0) == 0xffde2121u); // 34 sits among the black ones but takes the default
    CHECK(spell_target_color(35, 0) == 0xff000000u);
    CHECK(spell_target_color(49, 0) == 0xff5242deu); // the table's last entry
    CHECK(spell_target_color(50, 0) == 0xff000000u); // past it
}

TEST_CASE("Spell gem tints, tint stages and the cool-down rectangle (CSpellGemWnd)") {
    using hailstone::game::SpellGemDisplay;
    auto t = hailstone::game::spell_gem_tints(0xffde2121u);
    CHECK(t.colors[0] == 0xffde2121u);
    CHECK(t.colors[1] == 0xffca1e1eu); // an eleventh of each channel, truncated: 0x14, 3, 3
    CHECK(t.colors[10] == 0xffde2121u - 10u * 0x140303u);
    CHECK(t.alphas[0] == 0xffu);
    CHECK(t.alphas[10] == 0x2du);

    SpellGemDisplay d;
    d.refresh(false); // not ready: stage 1, one step darker a frame up to 10
    for (int i = 0; i < 12; ++i) d.process_frame();
    CHECK(d.stage == 1);
    CHECK(d.tint_index == 10);
    d.refresh(true); // ready after stage 1: stage 2 from 10 down
    CHECK(d.stage == 2);
    for (int i = 0; i < 10; ++i) d.process_frame();
    CHECK(d.tint_index == 0);
    CHECK(d.stage == 2);
    d.process_frame();
    CHECK(d.stage == 0);
    d.refresh(true); // ready and idle: nothing
    CHECK(d.stage == 0);

    // The cool-down: 5 s left of a 10 s recast is half done; it ends when its time is up.
    d.update_cooldown(100000, 5000, 0, 10000);
    REQUIRE(d.overlay_fraction(100000));
    CHECK(*d.overlay_fraction(100000) == doctest::Approx(0.5));
    // Within a second of what it shows, it is not restarted.
    d.update_cooldown(100500, 5200, 0, 10000);
    CHECK(d.cooldown_left(100500) == 4500);
    // 0.87 at most: the last 13 % stays covered until the end.
    CHECK(*d.overlay_fraction(104500) == 0.87);
    CHECK(hailstone::game::spell_gem_overlay_top(0.87, 10, 50) == 10 + 34);
    CHECK_FALSE(d.overlay_fraction(105000));
    CHECK(d.cooldown_duration == 0);
    // A linked timer longer than the gem's: its own time left is the total.
    d.update_cooldown(200000, 1000, 30000, 10000);
    CHECK(d.cooldown_duration == 30000);
    CHECK(hailstone::game::spell_gem_overlay_color(0xc00000u) == 0x40c00000u);
}

TEST_CASE("blocked buffs: the 128-byte RoF2 struct both ways, and spells found by name") {
    BlockedBuffs b;
    b.spells = {278, 1693};
    b.pet = true;
    b.initialise = true;
    auto p = encode(b);
    CHECK(p.opcode == to_wire(Op::BlockedBuffs));
    REQUIRE(p.data.size() == kBlockedBuffsSize);
    ByteReader r(p.data);
    CHECK(r.le<std::int32_t>() == 278);
    CHECK(r.le<std::int32_t>() == 1693);
    CHECK(r.le<std::int32_t>() == -1); // the unused ids
    auto back = decode_blocked_buffs(p);
    REQUIRE(back);
    CHECK(back->spells == std::vector<std::int32_t>{278, 1693});
    CHECK(back->pet);
    CHECK(back->initialise);
    CHECK(encode(b, true).opcode == to_wire(Op::RemoveBlockedBuffs));
    // The server's removal answer zeroes the unused ids; a count past them reads none.
    ByteWriter w;
    w.le<std::int32_t>(278);
    for (int k = 1; k < kBlockedBuffSlots; ++k) w.le<std::int32_t>(0);
    w.le<std::uint32_t>(3);
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(0);
    w.le<std::uint16_t>(0x54);
    auto removed = decode_blocked_buffs(packet(Op::RemoveBlockedBuffs, w));
    REQUIRE(removed);
    CHECK(removed->spells == std::vector<std::int32_t>{278});
    // Cmd_blockspell: the 30 slots as typed (-1 where an argument was no number), the count of the numbers.
    BlockedBuffs typed;
    typed.spells.assign(kBlockedBuffSlots, -1);
    typed.spells[0] = 1693;
    typed.spells[2] = 2517;
    typed.count = 2;
    auto tp = encode(typed);
    ByteReader tr(tp.data);
    CHECK(tr.le<std::int32_t>() == 1693);
    CHECK(tr.le<std::int32_t>() == -1);
    CHECK(tr.le<std::int32_t>() == 2517);
    for (int k = 3; k < kBlockedBuffSlots; ++k) tr.le<std::int32_t>();
    CHECK(tr.le<std::uint32_t>() == 2);
    auto clear = encode_clear_blocked_buffs(true);
    CHECK(clear.opcode == 0x34cb);
    REQUIRE(clear.data.size() == 1);
    CHECK(clear.data[0] == std::byte{1});

    game::SpellTable t;
    std::string text = spell_line({{0, "278"}, {1, "Spirit of Wolf"}}) + "\n" + spell_line({{0, "3185"}, {1, "Spirit of Wolf"}}) +
                       "\n" + spell_line({{0, "1693"}, {1, "Clarity"}}) + "\n";
    t.parse(text);
    REQUIRE(t.find_by_name("spirit of wolf"));
    CHECK(t.find_by_name("spirit of wolf")->id == 278); // the lowest id of the same name
    CHECK(t.find_by_name("CLARITY")->id == 1693);
    CHECK(!t.find_by_name("Clarit"));
}

TEST_CASE("auras: add, remove and clear (OP_UpdateAura)") {
    AuraUpdate a{kAuraAdd, 1, 1234, 77, "Circle of Power"};
    AppPacket p = encode(a);
    REQUIRE(p.data.size() == 80); // AuraCreate_Struct
    auto d = decode_aura_update(p);
    REQUIRE(d);
    CHECK(d->name == "Circle of Power");
    CHECK(d->spawn_id == 1234);
    CHECK(d->icon == 77);
    AppPacket r = encode(AuraUpdate{kAuraRemove, 0, 1234, 0, {}});
    REQUIRE(r.data.size() == 8); // AuraDestory_Struct, size checked exactly
    CHECK(decode_aura_update(r)->spawn_id == 1234);
    CHECK(encode(AuraUpdate{kAuraClear, 0, 0, 0, {}}).data.size() == 4);
}

TEST_CASE("OP_SpellEffect is the 28-byte SpellEffect_Struct; the nimbus packets are {u32 spawn, i32 effect}") {
    // EQEmu's Mob::SendSpellEffect for a nimbus: {effect, caster, caster, 500, 0, 3000, 1, 1, 0}.
    AppPacket p = encode(SpellEffectPacket{468, 1234, 1234, 500, 0, 3000, true, true, 0});
    CHECK(from_wire(p.opcode) == Op::SpellEffect);
    REQUIRE(p.data.size() == 28);
    ByteReader r(p.data);
    CHECK(r.le<std::uint32_t>() == 468);
    CHECK(r.le<std::uint32_t>() == 1234);
    CHECK(r.le<std::uint32_t>() == 1234);
    CHECK(r.le<std::uint32_t>() == 500);
    CHECK(r.le<std::uint32_t>() == 0);
    CHECK(r.le<std::uint32_t>() == 3000);
    CHECK(r.le<std::uint8_t>() == 1); // +0x18 full
    CHECK(r.le<std::uint8_t>() == 1); // +0x19 add
    auto d = decode_spell_effect(p);
    REQUIRE(d);
    CHECK(d->effect == 468);
    CHECK(d->stage2_length_ms == 3000);
    CHECK(d->full);
    CHECK(d->add);
    CHECK_FALSE(decode_spell_effect(AppPacket{p.opcode, std::vector<std::byte>(p.data.begin(), p.data.end() - 3)}));

    AppPacket add = encode(NimbusEffect{1234, 468}, true), remove = encode(NimbusEffect{1234, 468}, false);
    CHECK(from_wire(add.opcode) == Op::AddNimbusEffect);
    CHECK(from_wire(remove.opcode) == Op::RemoveNimbusEffect);
    REQUIRE(add.data.size() == 8);
    auto n = decode_nimbus_effect(remove);
    REQUIRE(n);
    CHECK(n->spawn_id == 1234);
    CHECK(n->effect == 468);
}

TEST_CASE("spells_us.txt: the live client's layout is told from RoF2's") {
    auto line = [](int carets) {
        std::string l = "1";
        for (int i = 0; i < carets; ++i) l += "^0";
        return l + "\n";
    };
    CHECK(game::spells_text_is_live_layout(line(165)));
    CHECK_FALSE(game::spells_text_is_live_layout(line(236)));
    CHECK_FALSE(game::spells_text_is_live_layout("")); // nothing to tell: not live
}

TEST_CASE("spells_us.txt: a live line read in RoF2's layout") {
    // Minor Healing as the live client has it (columns 4..: range, ...), its texts from spells_us_str.txt.
    std::string live = "200^Minor Healing^0^^100^0^0^0^1500^1500^1500^0^0^0^10";
    for (int c = 15; c < 30; ++c) live += c == 28 ? "^0" : "^0";
    live += "^5^0^0"; // 30 target type, 31 cast difficulty, 32 skill
    for (int c = 33; c < 75; ++c) live += c >= 36 && c < 52 ? (c == 37 ? "^1" : "^255") : "^0";
    live += "^99"; // 75 spell icon
    for (int c = 76; c < 165; ++c) live += "^0";
    live += "^1|0|10|0|2|20$3|15|5|0|100|0";
    std::string line = game::spell_line_from_live(
        live, {"", "", "You feel a little better.", " feels a little better.", ""});
    game::SpellTable table;
    REQUIRE(table.parse(line) == 1);
    const game::SpellData* s = table.get(200);
    REQUIRE(s);
    CHECK(s->name == "Minor Healing");
    CHECK(s->range == 100.0f);
    CHECK(s->cast_time == 1500);
    CHECK(s->mana == 10);
    CHECK(s->target_type == 5);
    CHECK(s->level_needed(2) == 1); // cleric: live column 37
    CHECK(s->spell_icon == 99);
    CHECK(s->cast_on_you == "You feel a little better.");
    CHECK(s->effect[0] == 0);
    CHECK(s->base[0] == 10);
    CHECK(s->formula[0] == 2);
    CHECK(s->max[0] == 20);
    CHECK(s->effect[1] == 254); // slot 2 empty
    CHECK(s->formula[1] == 100);
    CHECK(s->effect[2] == 15);
    CHECK(s->base[2] == 5);
    CHECK(s->deletable);
}

TEST_CASE("spells_us.txt: the live file read in RoF2's layout agrees with RoF2's") {
    // Runs with both installs: EQ_GAME_DIR a RoF2 one, EQ_LIVE_DIR a live one.
    const char* live_dir = std::getenv("EQ_LIVE_DIR");
    auto game = hailstone::test::game_dir();
    if (!live_dir || !game) return;
    GameDir live(live_dir);
    game::SpellTable rof2, converted;
    REQUIRE(rof2.load(*game));
    REQUIRE(converted.load(live));
    int both = 0, cast = 0, mana = 0, target = 0, icon = 0, effects = 0, levels = 0, texts = 0, range = 0;
    for (std::uint32_t id = 1; id <= game::kMaxSpellId; ++id) {
        const game::SpellData* a = rof2.get(id);
        const game::SpellData* b = converted.get(id);
        if (!a || !b || a->name != b->name) continue;
        ++both;
        cast += a->cast_time == b->cast_time;
        mana += a->mana == b->mana;
        target += a->target_type == b->target_type;
        icon += a->spell_icon == b->spell_icon;
        effects += a->effect == b->effect;
        levels += a->class_level == b->class_level;
        texts += a->cast_on_you == b->cast_on_you;
        range += a->range == b->range;
    }
    MESSAGE("spells in both: " << both << "; same cast time " << cast << ", mana " << mana << ", target " << target
                               << ", icon " << icon << ", effects " << effects << ", levels " << levels << ", texts "
                               << texts << ", range " << range);
    REQUIRE(both > 30000);
    // Live has changed some spells since RoF2; the columns are right when nearly all agree.
    CHECK(cast > both * 9 / 10);
    CHECK(mana > both * 9 / 10);
    CHECK(target > both * 9 / 10);
    CHECK(icon > both * 9 / 10);
    CHECK(effects > both * 8 / 10);
    CHECK(levels > both * 9 / 10);
    CHECK(texts > both * 9 / 10);
}

TEST_CASE("The scribe countdown takes 3 ticks only when the display clock moved on more than 1 ms") {
    // CSpellBookWnd::GetSpellScribeTicksLeft @0x75e620: TimeStamp - 0xf749ac > 1 (unsigned).
    game::Scribing sc;
    sc.ticks_left = 92;
    std::uint32_t last = 0;
    CHECK_FALSE(game::scribe_countdown(sc, last, 1000));
    CHECK(sc.ticks_left == 89);
    CHECK(last == 1000);
    // The same millisecond and the next: frames faster than 2 ms take nothing.
    CHECK_FALSE(game::scribe_countdown(sc, last, 1000));
    CHECK_FALSE(game::scribe_countdown(sc, last, 1001));
    CHECK(sc.ticks_left == 89);
    CHECK(last == 1000);
    CHECK_FALSE(game::scribe_countdown(sc, last, 1002));
    CHECK(sc.ticks_left == 86);
    // A clock behind `last` wraps to a large difference and ticks.
    CHECK_FALSE(game::scribe_countdown(sc, last, 500));
    CHECK(sc.ticks_left == 83);
    // 92 ticks: the 31st step is due; ticks and the clock go to 0.
    std::uint32_t now = 500;
    int steps = 3;
    while (!game::scribe_countdown(sc, last, now += 2)) ++steps;
    CHECK(steps + 1 == 31);
    CHECK(sc.ticks_left == 0);
    CHECK(last == 0);
}

TEST_CASE("GetPCSpellAffect stops at a spell's first blank slot; SpellAffects reads all 12") {
    // CharacterZoneClient::GetPCSpellAffect @0x433fd0 breaks at the first 0xfe; EQ_Spell::SpellAffects @0x4af770
    // does not.
    game::SpellData s;
    s.effect.fill(254);
    s.effect[0] = 0;
    s.effect[3] = 0x1b4; // after the blank slot 1
    CHECK(s.has_effect(0x1b4));
    CHECK_FALSE(s.has_effect_before_blank(0x1b4));
    CHECK(s.has_effect_before_blank(0));
    s.effect[1] = 0x1b4;
    CHECK(s.has_effect_before_blank(0x1b4));
}
