// EQ_Character::HitBySpell (game/mechanics/hit_by_spell.h) against the
// original's vectors (recorded from eqgame.exe): HitBySpell_Validation (300),
// _Instant (400), _Items (300), _Moves (300) and _Buff (500).
//
// Each record holds the inputs, every mocked callee the original called (in
// order, with its arguments and answer) and the fake virtuals' tape (the
// spell manager, the CharacterBase, spawns, EQGraphicsDX9's scene, collision
// and sine table), then the state after the call. The fakes' answers that the
// record does not hold (the CharacterBase's level / mana / PcClient, the floor
// heights, the sine-table values, the collision answers, DoSpellCheck's
// record bytes) come from the world the record was recorded over: it is drawn
// again below from the same random streams, and every regenerated input is
// first compared with the record's "in". The port then runs against a replay: each callee it calls must be the
// record's next call with the same arguments (byte arguments the original
// pushes as dwords compare by their low byte; a pointer the record names
// "world" is any object of the world), and gets the recorded answer; each
// virtual must be the tape's next entry. Finally every recorded output is
// compared, floats bit for bit.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <system_error>
#include <string>
#include <vector>

#include "game/mechanics/hit_by_spell.h"
#include "vector_files.h"

using namespace hailstone::game::mechanics;
using namespace hailstone::game::mechanics::hit;
using hailstone::test::Json;

namespace {

// ---------------------------------------------------------------- regenerating a record's world
//
// The records were recorded over worlds drawn from seeded xorshift32 streams: one per file, whose draws seed
// one stream per record. The draws below must come in the same order and with the same bounds as when the
// vectors were recorded, or the world no longer matches the record's "in".

class Stream {
public:
    explicit Stream(std::uint32_t seed) : s_(seed == 0 ? 0x9e3779b9u : seed) {}

    std::uint32_t bits() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return s_;
    }
    // lo..hi inclusive, by the remainder over the span (the span is taken unsigned, as when recorded).
    std::int32_t in(std::int32_t lo, std::int32_t hi) {
        const std::uint32_t span = static_cast<std::uint32_t>(hi) - static_cast<std::uint32_t>(lo) + 1u;
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(lo) + bits() % span);
    }
    std::uint32_t in_u(std::int32_t lo, std::int32_t hi) { return static_cast<std::uint32_t>(in(lo, hi)); }
    std::uint8_t in_byte(std::int32_t lo, std::int32_t hi) { return static_cast<std::uint8_t>(in(lo, hi)); }
    bool odds(std::int32_t percent) { return static_cast<std::int32_t>(bits() % 100) < percent; }
    // A quarter of the time an edge value, a quarter anything at all, else a small number.
    std::int32_t awkward() {
        static constexpr std::int32_t kEdge[] = {0, 1, -1, 2, 9, 10, 11, 59, 60, 61, 99, 100, 255, 256, 330, 1000,
                                                 0x7fffffff, static_cast<std::int32_t>(0x80000000u), 10000000,
                                                 9999999};
        switch (bits() % 4) {
        case 0: return kEdge[bits() % std::size(kEdge)];
        case 3: return static_cast<std::int32_t>(bits());
        default: return in(-5, 400);
        }
    }

private:
    std::uint32_t s_;
};

std::uint32_t bits_of(float f) { return std::bit_cast<std::uint32_t>(f); }

// Any float's bits, NaNs, infinities and denormals included (6 times in 14 a plain number).
std::uint32_t wild_float(Stream& s) {
    const std::int32_t kind = s.in(0, 13);
    if (kind >= 8)
        return bits_of(static_cast<float>(static_cast<std::int32_t>(s.bits() % 4000001) - 2000000) / 997.0f);
    if (kind == 2 || kind == 3) { // a NaN: quiet, or signalling (never a zero payload)
        const std::uint32_t payload = s.bits() & 0x3fffff;
        return kind == 2 ? 0x7fc00000 | payload : 0x7f800000 | payload | 1;
    }
    if (kind == 4) return s.odds(50) ? 0x7f800000 : 0xff800000; // an infinity
    if (kind == 5) return s.bits() & 0x807fffff;               // a denormal
    if (kind == 7) return s.bits();
    return kind == 0 ? 0u : kind == 1 ? 0x80000000u : 0x3f800000u; // 0, -0, 1
}
// A whole number lo..hi divided by `d`.
std::uint32_t fraction(Stream& s, std::int32_t lo, std::int32_t hi, float d) {
    return bits_of(static_cast<float>(s.in(lo, hi)) / d);
}
// 1.0f, or half the time the float just above it.
std::uint32_t one_or_next(Stream& s) { return s.odds(50) ? 0x3f800001 : 0x3f800000; }
// Mostly a number within +-1640, sometimes a wild one.
std::uint32_t ordinary_float(Stream& s) {
    if (s.odds(8)) return wild_float(s);
    return bits_of(static_cast<float>(static_cast<std::int32_t>(s.bits() % 200001) - 100000) / 61.0f);
}
std::int32_t small_value(Stream& s) {
    const std::int32_t kind = s.in(0, 5);
    if (kind == 0) return s.awkward();
    if (kind == 1) return -s.in(0, 300);
    if (kind == 2) return s.in(0, 300);
    return s.in(-20, 20);
}

// What a file's records exercise. Each family but the validation and buff ones lists the SPAs its main spell's
// first slot sweeps and its other slots favour; the order is the one the draws index.
enum class Set { Validation, Instant, Items, Moves, Buff };

constexpr std::int32_t kInstantSpas[] = {160, 20, 68, 25, 72, 189, 74, 42, 56, 15, 358, 417, 343, 91, 338, 388};
constexpr std::int32_t kItemSpas[] = {61, 62, 38, 35, 36, 116, 369, 389, 390};
constexpr std::int32_t kMoveSpas[] = {83, 145, 379, 380, 381};

std::span<const std::int32_t> listed_spas(Set set) {
    switch (set) {
    case Set::Instant: return kInstantSpas;
    case Set::Items: return kItemSpas;
    case Set::Moves: return kMoveSpas;
    default: return {};
    }
}
bool early(Set set) { return set == Set::Validation; }

// The SPAs HitBySpell's first effect loop handles: those three families' together.
bool first_loop_handles(std::int32_t spa) {
    for (Set set : {Set::Instant, Set::Items, Set::Moves}) {
        const auto spas = listed_spas(set);
        if (std::find(spas.begin(), spas.end(), spa) != spas.end()) return true;
    }
    return false;
}
std::int32_t unlisted_spa(Stream& s) {
    for (;;) {
        const std::int32_t spa = s.odds(3) ? static_cast<std::int32_t>(s.bits()) : s.in(-3, 470);
        if (!first_loop_handles(spa)) return spa;
    }
}
// The main spell's first SPA walks through the family's list and then -3..472 (a buff's through -3..471), one
// step per record: no draw.
std::int32_t swept_spa(Set set, std::uint32_t step) {
    if (set == Set::Buff) return static_cast<std::int32_t>(step % 475) - 3;
    const auto spas = listed_spas(set);
    const auto listed = static_cast<std::uint32_t>(spas.size());
    const std::uint32_t at = step % (listed + 476);
    if (at < listed) return spas[at];
    const std::int32_t spa = static_cast<std::int32_t>(at - listed) - 3;
    return first_loop_handles(spa) ? 254 : spa;
}
std::int32_t random_spa(Stream& s, Set set) {
    if (set == Set::Buff) return s.odds(5) ? static_cast<std::int32_t>(s.bits()) : s.in(-3, 470);
    const auto spas = listed_spas(set);
    if (!spas.empty() && s.odds(60)) return spas[static_cast<std::size_t>(s.in(0, static_cast<std::int32_t>(spas.size()) - 1))];
    return unlisted_spa(s);
}

void set_text(char* to, const char* text) { std::strcpy(to, text); }

void fill_spell(Stream& s, Spell& sp, Set set, std::uint32_t step, bool main) {
    sp.range = wild_float(s);
    if (s.odds(70)) sp.push_back = 0;
    else sp.push_back = s.odds(20) ? 0x80000000 : wild_float(s);
    sp.push_up = s.odds(70) ? 0 : wild_float(s);
    if (!main) sp.duration_formula = s.in_u(0, 2);
    else if (set == Set::Buff) sp.duration_formula = s.in_u(1, 9);
    else if (early(set) && s.odds(30)) sp.duration_formula = s.in_u(1, 3);
    else sp.duration_formula = 0;
    const int filled = s.odds(30) ? 12 : s.in(1, 12);
    for (std::size_t k = 0; k < 12; ++k) {
        std::int32_t spa = main && k == 0 ? swept_spa(set, step) : random_spa(s, set);
        if (static_cast<int>(k) >= filled && s.odds(80)) spa = 254;
        sp.attrib[k] = spa;
        sp.base[k] = s.odds(10) ? 0 : small_value(s);
        sp.base2[k] = small_value(s);
        sp.max[k] = small_value(s);
    }
    sp.recourse_178 = s.odds(60) ? 0 : s.in_u(1, 40);
    sp.timer_id_198 = s.odds(70) ? s.in_u(0, 0x15) : s.bits();
    sp.flag_240 = s.odds(80) ? 0 : 1;
    sp.spell_type = s.odds(80) ? s.in_byte(0, 1) : s.in_byte(2, 255);
    sp.byte_26c = static_cast<std::uint8_t>(s.bits());
    if (s.odds(30)) sp.target_type = 0xd;
    else sp.target_type = s.odds(30) ? 0x14 : s.in_byte(0, 30);
    static const char* const kNames[] = {"Spirit of Wolf", "Gate", "X"};
    set_text(sp.name.data(), kNames[s.in(0, 2)]);
    if (s.odds(70)) set_text(sp.teleport_zone.data(), s.odds(50) ? "qeynos" : "freportn");
}

// The fakes' answers. The replay takes most callees' answers from the record, so many of these are only drawn
// (the stream needs them); the ones the replay reads are marked.
struct Answers {
    // The CharacterBase and the spawns' virtuals (read).
    std::uint32_t has_pc = 0, cb_level = 0, cb_max_mana = 0, me_vf_dc = 0, caster_vf_dc = 0;
    // The scene and collision (read): the point in the world, the actor collision flag (low byte), the
    // terrain flag, which line tests hit (a bit per call), whether the sphere test writes its normal.
    std::uint32_t in_world = 0, actor_collision = 0, terrain_collision = 0, line_hits = 0, writes_normal = 0;
    // DoSpellCheck's record seed, FindAffectSlot's slot, GetItem's item, GetLoreName's text (read).
    std::uint32_t spell_check = 0, affect_slot = 0, item = 0, lore_text = 0;
    // The thrown-at spawn (moves; set to 0 before each run, read).
    std::uint32_t throw_at = 0;
    // Only drawn.
    std::uint32_t their_character = 0, bard = 0, stunned = 0, item_cast = 0, group = 0, max_effects = 0;
    std::uint32_t me_is_pc = 0, caster_is_pc = 0, pvp_duration = 0, duration = 0, caster_level = 0;
    std::uint32_t teleport_ok = 0, has_affect = 0, roll = 0, has_property = 0, get_150 = 0, bind_spot = 0;
    std::uint32_t expansion = 0, zone_index = 0, hton = 0, sent = 0, item_type = 0, book_lang = 0, magic = 0;
    std::uint32_t pc_affect = 0, time = 0, gem_seed = 0, focus = 0, random_100 = 0, low_high = 0, refused = 0;
    std::uint32_t total = 0, change_form = 0, generic = 0, has_flag = 0, vehicle = 0, mount = 0;
    // Three dwords no draw sets: they keep the World's size, which decides which stray pointers are "world".
    std::uint32_t spare[3]{};
};

// Everything the record's objects are, in one block (a pointer inside it that
// the record does not name is "world").
struct World {
    Character self;
    CharacterBase cb;
    Spawn me, caster, target, mount, vehicle;
    Character caster_char;
    // me's +0x2cc is the PcClient ("pc"); read as a CharacterZoneClient its +0x1a4 is 0 and its +8 (the
    // vbtable pointer) leads to zero bytes: a spawn of type 0.
    Character me_char;
    Spawn pc_as_spawn;
    Pc pc, caster_pc;
    char pc_name[64]{};
    std::array<std::int32_t, 12> timers{};
    Profile profile;
    std::int32_t drunkenness = 0; // profile +0x3300, for the PcBase mocks
    std::array<Spell, 8> spells;
    std::array<std::uint32_t, 8> spell_ids{};
    Hit hit;
    Launch launch;
    Affect affect;
    std::array<Affect, 10> buffs;
    char texts[4][0x30]{};
    Answers answers;
    std::array<std::int32_t, 16> calc{}, cached{};
    std::uint32_t floors[4][2]{};
    std::uint32_t trig[2]{};
    std::uint32_t normal[3]{};
    Globals globals;
    bool has_me = true;
    bool caster_none = false, caster_me = false;
    std::uint32_t dd00e4 = 0;
    // Run state.
    std::uint32_t floor_calls = 0, line_calls = 0, teleport_calls = 0;
    const Launch* launch_used = nullptr;
    std::int32_t duration = -1;
};

// The spell ids the main spell draws from half the time.
constexpr std::uint32_t kMainIds[] = {0x3d6, 0x29a, 0x2a2, 0x628, 0xe6e, 0xad7, 0x57, 0x58, 0xad6, 0x2e6, 0x5c4};

// Draws one record's world, section by section, in the stream's order.
class WorldDraw {
public:
    WorldDraw(World& w, Stream& s, Set set) : w_(w), s_(s), set_(set) {}

    void all(std::uint32_t step) {
        w_ = World{};
        w_.self.base = &w_.cb;
        character();
        spawn(w_.me);
        spawn(w_.caster);
        links();
        profile();
        spells(step);
        hit();
        buffs();
        affect();
        target_and_rides();
        answers();
        tables();
        globals();
    }

private:
    void character() {
        w_.has_me = !(early(set_) && s_.odds(2));
        w_.self.spawn = w_.has_me ? &w_.me : nullptr;
        w_.self.cache_valid = s_.in_byte(0, 1);
        w_.self.aa_cache_valid = s_.in_byte(0, 2);
        w_.self.stats_dirty = s_.in_byte(0, 2);
        CharacterBase& cb = w_.cb;
        cb.zone_id = s_.in(1, 4);
        for (std::uint32_t* v : {&cb.y, &cb.x, &cb.z, &cb.heading}) *v = ordinary_float(s_);
        cb.mesmerized = s_.in_byte(0, 2);
    }

    void spawn(Spawn& p) {
        p.type = s_.odds(75) ? 0 : s_.in_byte(1, 3);
        p.flag_208 = s_.odds(70) ? 0 : 1;
        p.owner_38c = s_.odds(70) ? 0 : s_.in_u(1, 3);
        p.spawn_id = s_.in_u(1, 3);
        for (std::uint32_t* v : {&p.y, &p.x, &p.z, &p.speed_y, &p.speed_x, &p.speed_z, &p.rotation_speed, &p.heading,
                                 &p.heading_speed, &p.floor_z, &p.model_height})
            *v = ordinary_float(s_);
        p.casting_spell = s_.odds(30) ? -1 : s_.in(0, 12);
        p.stand_state = s_.odds(50) ? 0x64 : s_.in_byte(0, 0x70);
        p.class_id = s_.odds(20) ? 8 : s_.in_byte(1, 16);
        s_.in(1, 100); // +0x250
    }

    void links() {
        w_.caster.character = s_.odds(75) ? &w_.caster_char : nullptr;
        w_.me.character = s_.odds(75) ? &w_.me_char : nullptr;
        w_.me_char.spawn = &w_.pc_as_spawn;
        w_.caster_char.spawn = &w_.caster;
        w_.caster_char.cache_valid = s_.in_byte(0, 1);
        w_.pc.gem_timers = s_.odds(80) ? w_.timers.data() : nullptr;
        w_.pc.id_20a8 = static_cast<std::int32_t>(s_.bits());
    }

    void profile() {
        Profile& p = w_.profile;
        if (s_.odds(60)) p.class_id = s_.in_byte(1, 16);
        else p.class_id = s_.odds(50) ? 8 : 7;
        p.weight = s_.odds(10) ? s_.awkward() : s_.in(0, 400);
        p.mana = small_value(s_);
        p.dword_3248 = static_cast<std::int32_t>(s_.bits());
        p.dword_3264 = static_cast<std::int32_t>(s_.bits());
        w_.drunkenness = s_.in(0, 250);
    }

    // The main spell (0), the 0x11a8 spell (1) and six others; each id is drawn before its spell.
    void spells(std::uint32_t step) {
        main_id_ = s_.odds(40) ? kMainIds[s_.in(0, 10)] : s_.in_u(100, 300);
        if (early(set_) && s_.odds(3)) main_id_ = s_.odds(50) ? 0 : 0xafc9;
        for (std::size_t k = 0; k < w_.spells.size(); ++k) {
            if (k == 0) w_.spell_ids[k] = main_id_;
            else if (k == 1) w_.spell_ids[k] = 0x11a8;
            else w_.spell_ids[k] = s_.in_u(1, 60);
            fill_spell(s_, w_.spells[k], set_, step, k == 0);
        }
        if (early(set_) && s_.odds(3)) w_.spell_ids[0] = main_id_ + 1; // the main spell no longer has the hit's id
    }

    std::uint32_t some_spell_id() { return w_.spell_ids[static_cast<std::size_t>(s_.in(0, 7))]; }

    void hit() {
        Hit& h = w_.hit;
        h.caster_id = static_cast<std::uint16_t>(s_.in(1, 9));
        h.item_level = static_cast<std::uint16_t>(s_.odds(60) ? 0 : s_.in(1, 300));
        if (s_.odds(40)) h.modifier = 0x3f800000;
        else h.modifier = s_.odds(30) ? fraction(s_, 1, 40, 8.0f) : wild_float(s_);
        h.push_heading = s_.odds(60) ? ordinary_float(s_) : wild_float(s_);
        h.word_1b = static_cast<std::uint16_t>(s_.odds(50) ? 0 : some_spell_id());
        h.song_value = small_value(s_);
        h.spell_id = main_id_;
        h.caster_level = s_.odds(20) ? s_.in_byte(200, 255) : s_.in_byte(1, 100);
        w_.launch.byte_32 = static_cast<std::uint8_t>(s_.bits());
        w_.launch.item_flag = s_.in_byte(0, 1);
    }

    void buffs() {
        for (Affect& b : w_.buffs) {
            b.type = s_.odds(70) ? s_.in_byte(1, 4) : 0;
            b.level = s_.in_byte(1, 110);
            b.spell_id = static_cast<std::int32_t>(s_.odds(80) ? some_spell_id() : s_.bits() % 100);
            for (std::int32_t& v : b.slot_data) v = s_.in(-3, 10);
            b.flags_24 = s_.bits();
        }
    }

    // The affect HitBySpell may fill: 24 raw dwords, of which the first 22 are its fields.
    void affect() {
        std::array<std::uint32_t, 24> raw{};
        for (std::uint32_t& v : raw) v = s_.bits();
        Affect& a = w_.affect;
        a.type = static_cast<std::uint8_t>(raw[0]);
        a.level = static_cast<std::uint8_t>(raw[0] >> 8);
        a.byte_2 = static_cast<std::uint8_t>(raw[0] >> 16);
        a.byte_3 = static_cast<std::uint8_t>(raw[0] >> 24);
        a.modifier = raw[1];
        a.spell_id = static_cast<std::int32_t>(raw[2]);
        a.duration = static_cast<std::int32_t>(raw[3]);
        a.caster_10 = static_cast<std::int32_t>(raw[4]);
        a.dword_14 = static_cast<std::int32_t>(raw[5]);
        a.y_18 = raw[6];
        a.x_1c = raw[7];
        a.z_20 = raw[8];
        a.flags_24 = raw[9];
        for (std::size_t j = 0; j < a.slot_data.size(); ++j) a.slot_data[j] = static_cast<std::int32_t>(raw[10 + j]);
    }

    // The target, then the mount's and the vehicle's motion: a pair of draws for each dword +0x70..+0x8c, of
    // which +0x84 and +0x88 are not kept.
    void target_and_rides() {
        w_.target.type = s_.in_byte(0, 3);
        w_.target.owner_38c = s_.in_u(1, 3);
        w_.target.spawn_id = s_.bits();
        set_text(w_.target.name.data(), "Target");
        using Field = std::uint32_t Spawn::*;
        static constexpr Field kMotion[] = {&Spawn::speed_y, &Spawn::speed_x, &Spawn::speed_z, &Spawn::rotation_speed,
                                            &Spawn::heading, nullptr, nullptr, &Spawn::heading_speed};
        for (Field f : kMotion) {
            const std::uint32_t for_mount = ordinary_float(s_);
            const std::uint32_t for_vehicle = ordinary_float(s_);
            if (f == nullptr) continue;
            w_.mount.*f = for_mount;
            w_.vehicle.*f = for_vehicle;
        }
        static const char* const kTexts[] = {"~*Lore Book", "#Book", "Plain Book", "Elvish"};
        for (int t = 0; t < 4; ++t) set_text(w_.texts[t], kTexts[(t + s_.in(0, 3)) & 3]);
    }

    void answers() {
        if (early(set_) && s_.odds(3)) w_.caster_none = true;
        else w_.caster_me = s_.odds(30);
        Answers& a = w_.answers;
        a.their_character = s_.odds(40) ? 0 : 1; // 0: pc, 1: pc + 0x100 (the caster's)
        a.bard = s_.odds(30) ? 1 : 0;
        a.stunned = early(set_) && s_.odds(10) ? 1 : 0;
        a.item_cast = s_.odds(30);
        a.group = s_.odds(70);
        const std::uint32_t check_high = s_.bits() & ~1u;
        a.spell_check = check_high | (s_.odds(95) ? 1u : 0u);
        a.max_effects = s_.in_u(0, 10);
        a.me_is_pc = s_.odds(50);
        a.caster_is_pc = s_.odds(50);
        a.pvp_duration = static_cast<std::uint32_t>(small_value(s_));
        a.duration = static_cast<std::uint32_t>(small_value(s_));
        a.caster_level = s_.odds(50) ? s_.in_u(30, 60) : s_.in_u(0, 255);
        a.cb_level = s_.in_u(30, 60);
        a.cb_max_mana = static_cast<std::uint32_t>(small_value(s_));
        a.has_pc = s_.odds(85) ? 1 : 0;
        a.me_vf_dc = s_.odds(50);
        a.caster_vf_dc = s_.odds(50);
        a.in_world = s_.odds(85);
        const std::uint32_t enabled = s_.in_u(0, 1);
        a.actor_collision = enabled | (s_.bits() & 0xffffff00u);
        a.terrain_collision = s_.odds(60);
        a.line_hits = s_.bits();
        a.teleport_ok = s_.bits();
        a.affect_slot = s_.in_u(0, 60);
        a.has_affect = s_.odds(90) ? 1 : 0;
        if (!s_.odds(40)) a.roll = s_.in_u(1, 100);
        else a.roll = s_.odds(50) ? s_.in_u(94, 96) : s_.in_u(4, 6);
        a.has_property = s_.odds(30);
        a.get_150 = s_.odds(15);
        a.bind_spot = s_.odds(60);
        a.expansion = s_.odds(60);
        a.zone_index = s_.in_u(1, 4);
        a.hton = s_.bits();
        a.sent = s_.odds(80);
        a.item = s_.odds(80) ? 0x1234500u + s_.in_u(0, 3) : 0;
        a.item_type = s_.in_u(0, 2);
        a.book_lang = s_.in_u(0, 5);
        a.magic = s_.odds(50);
        for (std::uint32_t* v : {&a.pc_affect, &a.time, &a.gem_seed, &a.focus}) *v = s_.bits();
        a.random_100 = s_.in_u(0, 100);
        a.low_high = s_.odds(40) ? s_.in_u(9, 11) : s_.in_u(1, 100);
        a.refused = s_.odds(60) ? 0 : s_.in_u(1, 2);
        a.total = s_.odds(50) ? 0 : static_cast<std::uint32_t>(small_value(s_));
        a.change_form = s_.odds(85);
        a.generic = static_cast<std::uint32_t>(small_value(s_));
        a.has_flag = s_.odds(50);
        a.vehicle = s_.odds(30);
        a.mount = s_.odds(40);
        a.writes_normal = s_.odds(60);
        a.throw_at = 0;
        a.lore_text = s_.in_u(0, 3);
    }

    // The effect formulas and caches, the floor heights (value, scale), the sine-table answer (value, scale) and
    // the collision normal.
    void tables() {
        for (std::size_t j = 0; j < w_.calc.size(); ++j) {
            w_.calc[j] = s_.odds(35) ? s_.in(-3, 9) : small_value(s_);
            w_.cached[j] = s_.odds(70) ? 0 : small_value(s_);
        }
        for (auto& floor : w_.floors) {
            if (s_.odds(25)) {
                floor[0] = 0xec4ecb8f; // no floor
                floor[1] = 0x3f800000;
                continue;
            }
            floor[0] = ordinary_float(s_);
            floor[1] = one_or_next(s_);
        }
        if (s_.odds(80)) w_.trig[0] = fraction(s_, -1000, 1000, 1000.0f);
        else w_.trig[0] = wild_float(s_);
        w_.trig[1] = one_or_next(s_);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (axis == 0 && s_.odds(20)) w_.normal[axis] = 0x7f800001 | (s_.bits() & 0x3ffffe); // a signalling NaN
            else w_.normal[axis] = ordinary_float(s_);
        }
    }

    void globals() {
        Globals& g = w_.globals;
        g.target = s_.odds(80) ? &w_.target : nullptr;
        Spawn* const anyone[] = {&w_.me, &w_.caster, &w_.target, nullptr};
        g.local_player = anyone[s_.in(0, 3)];
        g.controlled_player = anyone[s_.in(0, 3)];
        g.connected = s_.odds(70);
        g.zone_type = s_.in_byte(0, 5);
        g.ruleset = s_.odds(50) ? 0 : s_.in_u(1, 7);
        g.bind_blocked = s_.odds(85) ? 0 : 1;
        g.no_send_log = s_.in_byte(0, 1);
        const std::uint32_t pushed = s_.bits() & 0xfefefefe;
        g.controlled_pushed = static_cast<std::uint8_t>(pushed);
        g.local_pushed = static_cast<std::uint8_t>(pushed >> 8);
        g.save_reason = static_cast<std::int32_t>(s_.bits());
        // The world packet buffer at 0xdd00e0: its first dword, then the next.
        const std::uint32_t buffer_0 = s_.bits();
        w_.dd00e4 = s_.bits();
        g.packet_opcode = static_cast<std::uint16_t>(buffer_0);
        g.packet_target = static_cast<std::int32_t>((buffer_0 >> 16) | (w_.dd00e4 << 16));
        g.send_count = static_cast<std::int32_t>(s_.bits());
    }

    World& w_;
    Stream& s_;
    Set set_;
    std::uint32_t main_id_ = 0;
};

// Per-file adjustments after the world, drawn from the same stream.

// Validation: sometimes an unknown SPA (123) in a detrimental spell, sometimes with the formulas near 0.
void validation_extra(World& w, Stream& s) {
    if (!s.odds(30)) return;
    Spell& main = w.spells[0];
    main.attrib[static_cast<std::size_t>(s.in(0, 11))] = 123;
    main.spell_type = 0;
    if (!s.odds(60)) return;
    for (std::size_t j = 0; j < w.calc.size(); ++j) {
        w.cached[j] = 0;
        w.calc[j] = s.in(-1, 1);
    }
}

// Buffs: the SPAs whose values the buff path scales or not, and formula pairs.
void buff_extra(World& w, Stream& s) {
    Spell& main = w.spells[0];
    static constexpr std::int32_t kFavoured[] = {0x3a, 0x9c, 0x1e, 0x11, 0x12, 0x56, 35, 36, 116, 369, 3, 156};
    for (std::size_t i = 1; i < 12; ++i)
        if (s.odds(25)) main.attrib[i] = kFavoured[s.in(0, 11)];
    if (s.odds(15)) {
        static constexpr std::int32_t kUnscaled[] = {0x3a, 0x9c, 0x1e, 0x11, 0x12, 0x56, 254};
        static constexpr std::int32_t kLead[] = {0x56, 0x12, 3, 0x9c, 200};
        for (std::size_t i = 1; i < 12; ++i) main.attrib[i] = kUnscaled[s.in(0, 6)];
        main.attrib[0] = kLead[s.in(0, 4)];
    }
    if (s.odds(30)) {
        static constexpr std::int32_t kFormulas[] = {-2, -1, 8, 9, 7, 6};
        for (std::int32_t& c : w.calc) c = kFormulas[s.in(0, 5)];
    }
    if (s.odds(30)) w.hit.modifier = fraction(s, 2, 40, 8.0f);
}

// Moves: shadow step (SPA 379) distances, and a thrown-at spawn.
void moves_extra(World& w, Stream& s) {
    Spell& main = w.spells[0];
    for (std::size_t i = 0; i < 12; ++i) {
        if (main.attrib[i] != 379) continue;
        main.base[i] = s.odds(10) ? -s.in(0, 0x7fffffff) : s.in(-2, 16);
    }
    w.answers.throw_at = s.odds(6) ? s.in_u(1, 6) : 0;
}

using Extra = void (*)(World&, Stream&);

// One record's world: the draws, the file's extra, then the launch record and the duration override.
void draw_record(World& w, Stream& s, Set set, Extra extra, std::uint32_t step) {
    WorldDraw(w, s, set).all(step);
    if (extra != nullptr) extra(w, s);
    w.answers.throw_at = 0;
    const bool without_launch = s.odds(2) && early(set);
    w.launch_used = without_launch ? nullptr : &w.launch;
    w.duration = s.odds(80) ? -1 : small_value(s);
}

// ---------------------------------------------------------------- names

constexpr std::uint32_t kExeBase = 0x1000000; // where the recording loaded eqgame.exe
std::uint32_t unrebase(std::uint32_t v) {
    return v >= kExeBase && v < kExeBase + 0x1200000 ? v - kExeBase + 0x400000 : v;
}

// A value as the record writes it: the name of the object it points at, or the integer.
struct Named {
    std::optional<std::string> name;
    std::uint32_t value = 0;
};

class Names {
public:
    explicit Names(World& w) : w_(w) {
        add(&w.me, "me");
        add(&w.caster, "caster");
        for (int k = 0; k < 8; ++k) add(&w.spells[static_cast<std::size_t>(k)], "spell" + std::to_string(k));
        for (int k = 0; k < 10; ++k) add(&w.buffs[static_cast<std::size_t>(k)], "buff" + std::to_string(k));
        add(&w.affect, "affect");
        add(&w.pc, "pc");
        add(&w.me_char, "pc");
        add(&w.caster_pc, "caster_pc");
        by_value_[reinterpret_cast<std::uintptr_t>(&w.pc) + kPcCharacter] = "pc_character";
        by_value_[reinterpret_cast<std::uintptr_t>(&w.caster_pc) + kPcCharacter] = "caster_pc_character";
        add(&w.target, "target");
        add(&w.self, "self");
        add(&w.cb, "cb");
        add(&w.caster_char, "caster_character");
        add(&w.vehicle, "vehicle");
        add(&w.mount, "mount");
    }
    Named of(std::uintptr_t v) const {
        if (auto it = by_value_.find(v); it != by_value_.end()) return {it->second, 0};
        auto lo = reinterpret_cast<std::uintptr_t>(&w_);
        if (v >= lo && v < lo + sizeof(World)) return {std::string("world"), 0};
        return {std::nullopt, unrebase(static_cast<std::uint32_t>(v))};
    }
    bool in_world(const void* p) const {
        auto v = reinterpret_cast<std::uintptr_t>(p), lo = reinterpret_cast<std::uintptr_t>(&w_);
        return v >= lo && v < lo + sizeof(World);
    }
    std::uintptr_t object(const std::string& name) const {
        if (name == "pc") return reinterpret_cast<std::uintptr_t>(&w_.pc);
        for (const auto& [v, n] : by_value_)
            if (n == name) return v;
        return 0;
    }

private:
    template <typename T> void add(T* p, const std::string& n) { by_value_[reinterpret_cast<std::uintptr_t>(p)] = n; }
    World& w_;
    std::map<std::uintptr_t, std::string> by_value_;
};

// ---------------------------------------------------------------- the replay

struct Arg {
    enum Kind { Int, Byte, Ptr, Text, Skip } kind = Skip;
    std::uint32_t v = 0;
    std::uintptr_t p = 0;
    const char* t = nullptr;
};
Arg I(std::int64_t v) { return {Arg::Int, static_cast<std::uint32_t>(v), 0, nullptr}; }
Arg B(std::uint32_t v) { return {Arg::Byte, v, 0, nullptr}; }
template <typename T> Arg P(T* p) { return {Arg::Ptr, 0, reinterpret_cast<std::uintptr_t>(p), nullptr}; }
Arg H(Handle h) { return {Arg::Ptr, 0, h, nullptr}; }
Arg T(const char* t) { return {Arg::Text, 0, 0, t}; }
Arg S() { return {}; }

class Replay final : public HitQueries {
public:
    Replay(World& w, const Json& out, const Fpu& fpu) : w_(w), names_(w), fpu_(fpu) {
        for (const Json& c : out["calls"].items) {
            const std::string& fn = c["fn"].text;
            if (fn == "HashTableInt ctor" || fn == "HashTableInt dtor" || fn == "operator_delete" ||
                fn == "collision info dtor")
                continue;
            calls_.push_back(&c);
        }
        for (const Json& v : out["virtuals"].items) tape_.push_back(&v);
    }
    std::string error;
    HitFrame frame;
    std::string finish() {
        // Texts in the frame are what the frame holds at the end (the recording reads them then).
        for (const auto& [rec, ptr] : deferred_)
            if (error.empty() && *rec != ptr) error = "frame text " + std::string(ptr) + " recorded " + *rec;
        if (!error.empty()) return error;
        if (call_ < calls_.size()) return "call " + (*calls_[call_])["fn"].text + " not made";
        if (tape_at_ < tape_.size()) return "virtual " + (*tape_[tape_at_])[0].text + " not made";
        return "";
    }
    const Names& names() const { return names_; }

    // ---- virtuals
    const Spell* spell_by_id(std::uint32_t id) override {
        tape("SpellManager::GetSpellByID(id)", {id});
        for (std::size_t k = 0; k < 8; ++k)
            if (w_.spell_ids[k] == id) return &w_.spells[k];
        return nullptr;
    }
    Handle cb_pc() override {
        tape("CharacterBase::vf4c (PcClient)", {});
        return w_.answers.has_pc ? reinterpret_cast<Handle>(&w_.pc) : 0;
    }
    std::uint32_t cb_level() override {
        tape("CharacterBase::vf68 (level)", {});
        return w_.answers.cb_level;
    }
    std::uint32_t cb_max_mana() override {
        tape("CharacterBase::vf6c (max mana)", {});
        return w_.answers.cb_max_mana;
    }
    std::uint8_t spawn_vf_dc(Spawn* s) override {
        bool me = s == &w_.me;
        tape("spawn vfdc (is_me)", {me ? 1u : 0u});
        return static_cast<std::uint8_t>(me ? w_.answers.me_vf_dc : w_.answers.caster_vf_dc);
    }
    void pc_base_vf58(Handle base) override {
        tape("PcClient base vf58", {base == reinterpret_cast<Handle>(&w_.pc) + kPcBase ? 1u : 0u});
    }
    std::uint8_t point_in_world(const std::array<std::uint32_t, 3>& p) override {
        tape("pinstSceneGraph vf4c IsPointInWorld(x,y,z)", {p[0], p[1], p[2]});
        return static_cast<std::uint8_t>(w_.answers.in_world);
    }
    std::uint8_t actor_collision_enabled() override {
        tape("pinstCollision vf48 IsActorCollisionEnabled", {});
        return static_cast<std::uint8_t>(w_.answers.actor_collision);
    }
    void set_actor_collision(std::uint8_t flag) override {
        tape("pinstCollision vf50 SetActorCollisionEnabled(flag)", {flag}, {0xff});
    }
    std::uint8_t collide_sphere(CollisionSphere& s) override {
        const CollisionInfo& i = s.info;
        tape("pinstCollision vf08 CollideSphere: vtable, from x,y,z, to x", {i.vtable, i.from[0], i.from[1], i.from[2], i.to[0]});
        tape("  sphere: to y,z, radius, r^2, 1/r", {i.to[1], i.to[2], s.radius, s.radius_sq, s.inverse_radius});
        tape("  sphere: filter, callback, +0x94, +0x98, +0x9c", {s.filter, s.callback, s.dword_94, s.dword_98, s.byte_9c});
        tape("  sphere before the test: normal x,y,z (stale frame bytes), +0x34, +0x68",
             {i.normal[0], i.normal[1], i.normal[2], i.inverse_length, i.moving});
        if (w_.answers.writes_normal) s.info.normal = {w_.normal[0], w_.normal[1], w_.normal[2]};
        return 1;
    }
    std::uint8_t terrain_collision_enabled() override {
        tape("pinstCollision vf44 IsTerrainCollisionEnabled", {});
        return static_cast<std::uint8_t>(w_.answers.terrain_collision);
    }
    std::uint8_t collide_line(CollisionLine& l) override {
        const CollisionInfo& i = l.info;
        tape("pinstCollision vf00 CollideLineSegment: vtable, from x,y,z, to x", {i.vtable, i.from[0], i.from[1], i.from[2], i.to[0]});
        tape("  line: to y,z, mask +0x64, +0x68, +0x8c",
             {i.to[1], i.to[2], static_cast<std::uint32_t>(i.mask_64), i.moving, l.byte_8c});
        tape("  line: +0x74..+0x84", {l.ray[0], l.ray[1], l.ray[2], l.ray[3], l.ray[4]});
        tape("  line: +0x88, 1/len +0x34, t +0x38, dir x,y", {l.ray[5], i.inverse_length, i.fraction, i.direction[0], i.direction[1]});
        std::uint32_t n = w_.line_calls++;
        return static_cast<std::uint8_t>((w_.answers.line_hits >> (n & 31)) & 1);
    }
    double floor_height(Spawn* s, std::uint32_t x, std::uint32_t y, std::uint32_t z) override {
        tape("GetPlayerFloorHeight(is_me, x, y, z, flag)", {s == &w_.me ? 1u : 0u, x, y, z, 1});
        const std::uint32_t* f = w_.floors[w_.floor_calls++ & 3];
        return x87_mul(fpu_, X87(std::bit_cast<float>(f[0])), X87(std::bit_cast<float>(f[1])));
    }
    double table_cos(std::uint32_t angle) override {
        tape("sine table vf0c cos(table, angle)", {1, angle});
        return trig();
    }
    double table_sin(std::uint32_t angle) override {
        tape("sine table vf08 sin(table, angle)", {1, angle});
        return trig();
    }

    // ---- functions
    Spawn* spawn_by_id(std::uint32_t id) override {
        return obj<Spawn>(call("GetSpawnByID", std::nullopt, {I(id)}));
    }
    void recache_spell_effects(Character* c) override { call("ReCacheSpellEffects", P(c), {}); }
    std::int32_t cached_buff_effect(Character* c, std::int32_t spa) override {
        return ret(call("GetCachedBuffEffect", P(c), {I(spa), I(0)}));
    }
    std::int32_t cached_aa_effect(Character* c, std::int32_t spa) override {
        return ret(call("GetCachedAAEffect", P(c), {I(spa), I(0)}));
    }
    std::int32_t cached_item_effect(Character* c, std::int32_t spa) override {
        return ret(call("GetCachedItemEffect", P(c), {I(spa), I(0)}));
    }
    std::uint32_t bard_cast_bard(const Spell* s, std::uint32_t cls) override {
        return static_cast<std::uint32_t>(ret(call("BardCastBard", P(&w_.self), {P(s), I(cls)})));
    }
    std::uint8_t sub_00433830(const Spell* s, Character* c) override {
        return static_cast<std::uint8_t>(ret(call("sub_00433830", P(&w_.self), {P(s), P(c)})));
    }
    Handle character_of(Spawn* s) override { return handle(call("0x59f730", P(s), {})); }
    void token_to_client(Handle to, std::uint32_t id, std::uint32_t color) override {
        call("token_string_to_client", std::nullopt, {H(to), I(id), I(color), I(0)});
    }
    std::uint8_t is_item_cast(const Launch* l) override {
        return static_cast<std::uint8_t>(ret(call("IsItemCast", P(l), {})));
    }
    std::uint8_t is_self_or_group_member(Handle pc, const char* name) override {
        return static_cast<std::uint8_t>(ret(call("IsSelfOrGroupMember", H(pc), {T(name)})));
    }
    const char* pc_name(Handle pc) override {
        return pc == reinterpret_cast<Handle>(&w_.pc) || pc == reinterpret_cast<Handle>(&w_.caster_pc) ? w_.pc_name
                                                                                                          : "";
    }
    std::uint32_t do_spell_check(std::uint32_t id, SpellCheckRecord& r) override {
        const Json* c = call("DoSpellCheck", std::nullopt, {I(id), S(), I(1)});
        std::uint32_t seed = w_.answers.spell_check;
        for (std::uint32_t i = 0; i < 0x144; ++i) r[i] = static_cast<std::uint8_t>((seed >> 8) + i * 13 + (i >> 3));
        return static_cast<std::uint32_t>(ret(c));
    }
    void log_exit_game() override {
        call("DebugLog_Printf", std::nullopt, {I(0x9c5900), I(0x9c5948), S()});
        tape("DebugLog_Printf line", {0x395b});
    }
    void execute_cmd(std::uint32_t cmd) override { call("__ExecuteCmd", std::nullopt, {I(cmd), I(0), I(0)}); }
    std::uint8_t max_effects(Character* c) override {
        return static_cast<std::uint8_t>(ret(call("GetMaxEffects", P(c), {})));
    }
    Affect* get_effect(Character* c, std::uint32_t i) override {
        return obj<Affect>(call("GetEffect", P(c), {I(i)}));
    }
    void remove_affect(Affect* a) override { call("RemoveAffect", P(&w_.self), {P(a), I(1), I(0)}); }
    void cur_hp(std::uint32_t type) override { call("Cur_HP", P(&w_.self), {I(type), I(1)}); }
    std::uint8_t is_pc_or_mercenary(Spawn* s) override {
        return static_cast<std::uint8_t>(ret(call("Spawn_IsPCOrMercenary", std::nullopt, {P(s)})));
    }
    std::int32_t pvp_spell_duration(const Spell* s, std::uint32_t level) override {
        return ret(call("PvPSpellDuration", P(&w_.self), {P(s), B(level), I(0)}));
    }
    std::int32_t spell_duration(const Spell* s, std::uint32_t level) override {
        return ret(call("SpellDuration", P(&w_.self), {P(s), B(level), I(0)}));
    }
    std::int32_t calc_affect_change(const Spell* s, std::uint32_t level, std::uint32_t index, Spawn* caster,
                                    std::uint32_t flag) override {
        return ret(call("CalcAffectChange", P(&w_.self),
                        {P(s), B(level), B(index), I(0), I(-1), P(caster), I(0), I(-1), I(flag)}));
    }
    void build_string(char* out, std::uint32_t id, const char* arg) override {
        call("build_token_string_PARAM", std::nullopt,
             {S(), I(id), T(arg), I(0), I(0), I(0), I(0), I(0), I(0), I(0), I(0)});
        std::snprintf(out, 6, "B%04x", id & 0xffff);
    }
    void dsp_chat(const char* text, std::uint32_t color) override {
        call("dsp_chat", std::nullopt, {T(text), I(color), I(1), I(1)});
    }
    void dsp_chat_2(const char* text) override { call("dsp_chat_2", std::nullopt, {T(text)}); }
    std::uint8_t get_level(Spawn* s) override {
        return static_cast<std::uint8_t>(ret(call("PlayerZoneClient::GetLevel", P(s), {})));
    }
    Profile* current_profile() override {
        call("GetCurrentProfile", std::nullopt, {});
        return &w_.profile;
    }
    void record_position_sample(Spawn* s, std::uint32_t kind) override {
        call("RecordPositionSample", P(s), {I(kind), I(0), I(0)});
    }
    Spawn* vehicle_of(Spawn* s) override { return obj<Spawn>(call("0x58cf90", P(s), {})); }
    Spawn* mount_spawn(Spawn* s) override { return obj<Spawn>(call("GetMountSpawn", P(s), {})); }
    std::int32_t get_drunkenness(Handle pc) override { return ret(call("GetDrunkenness", H(pc), {})); }
    void set_drunkenness(Handle pc, std::int32_t v) override {
        call("SetDrunkenness", H(pc), {I(v)});
        w_.drunkenness = v;
    }
    std::int32_t random_percent() override { return ret(call("RandomPercentRoll", std::nullopt, {})); }
    std::uint8_t has_property(Spawn* s, std::uint32_t prop) override {
        return static_cast<std::uint8_t>(ret(call("HasProperty", P(s), {I(prop), I(0), I(0)})));
    }
    std::uint32_t get_150(Spawn* s) override {
        return static_cast<std::uint32_t>(ret(call("unk_PlayerClient_Get150", P(s), {})));
    }
    std::uint32_t bind_spot_ok(Spawn* where) override {
        return static_cast<std::uint32_t>(ret(call("sub_00432e90", P(&w_.self), {P(where)})));
    }
    std::uint32_t expansion_zone(std::int32_t zone) override {
        return static_cast<std::uint32_t>(ret(call("ExpansionZone", std::nullopt, {I(zone)})));
    }
    void set_bind_zone(Handle pc, std::uint32_t slot, std::int32_t zone) override {
        call("SetBindZoneID", H(pc), {I(slot), I(zone)});
    }
    void set_bind_y(Handle pc, std::uint32_t slot, std::uint32_t v) override { call("SetBindY", H(pc), {I(slot), I(v)}); }
    void set_bind_x(Handle pc, std::uint32_t slot, std::uint32_t v) override { call("SetBindX", H(pc), {I(slot), I(v)}); }
    void set_bind_z(Handle pc, std::uint32_t slot, std::uint32_t v) override { call("SetBindZ", H(pc), {I(slot), I(v)}); }
    void set_bind_heading(Handle pc, std::uint32_t slot, std::uint32_t v) override {
        call("SetBindHeading", H(pc), {I(slot), I(v)});
    }
    void save_pc() override { call("CEverQuest::SavePC", std::nullopt, {I(0), I(1), I(0xe)}); }
    void set_150(Spawn* s) override { call("0x8cfd10", P(s), {I(0)}); }
    void play_sound(Spawn* s, std::uint32_t sound) override { call("PlaySound", P(s), {I(sound)}); }
    std::int32_t zone_index(const char* name) override {
        return ret(call("GetIndexFromZoneName", std::nullopt, {T(name)}));
    }
    void move_to_zone(std::int32_t zone, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::int32_t heading) override {
        call("MoveToZone", std::nullopt,
             {I(zone), I(0x9c58ec), I(0), I(7), I(x), I(y), I(z), I(heading), I(-1), I(-1), I(0)});
    }
    void update_player_actor(Spawn* s) override { call("UpdatePlayerActor", P(s), {}); }
    void sub_00432bc0(std::uint32_t range) override { call("sub_00432bc0", P(&w_.self), {I(range)}); }
    std::uint16_t hton(std::uint32_t op) override {
        return static_cast<std::uint16_t>(ret(call("hton", std::nullopt, {I(op), I(0)})));
    }
    std::uint8_t send_world(std::uint32_t size) override {
        return static_cast<std::uint8_t>(ret(call("UdpConnection::Send", std::nullopt, {I(4), I(0xdd00e0), I(size)})));
    }
    void close_world() override { call("NetMgr_CloseConnection", std::nullopt, {I(0xdd25ac)}); }
    void log_void_connection(std::uint32_t op) override {
        call("DebugLog_Printf", std::nullopt, {I(0x9c3ed8), I(op), S()});
    }
    const char* get_string(std::uint32_t id) override {
        call("getString", std::nullopt, {I(id), I(0)});
        return w_.texts[(id ^ (id >> 4)) & 3];
    }
    void set_endurance(std::int32_t v) override { call("0x441e50", P(&w_.cb), {I(v)}); }
    void cur_endurance() override { call("Cur_Endurance", P(&w_.self), {I(1)}); }
    void cur_mana() override { call("Cur_Mana", P(&w_.self), {I(1)}); }
    void feign(std::uint32_t id, std::int32_t v) override { call("StopCastAndFeignDeath", P(&w_.self), {I(id), I(v)}); }
    Handle get_item(std::uint32_t slot) override {
        call("GetItem", P(&w_.cb), {S(), I(slot)});
        return w_.answers.item;
    }
    void release_item() override { call("VePointer::~VePointer", std::nullopt, {}); }
    const char* lore_name(Handle item) override {
        call("GetLoreName", H(item), {});
        return w_.texts[w_.answers.lore_text & 3];
    }
    std::int32_t item_type(Handle item) override { return ret(call("GetType", H(item), {})); }
    std::uint32_t book_lang(Handle item) override {
        return static_cast<std::uint32_t>(ret(call("GetBookLang", H(item), {})));
    }
    const char* lang_desc(std::uint32_t lang) override {
        call("GetLangDesc", std::nullopt, {I(lang)});
        return w_.texts[lang & 3];
    }
    std::uint8_t is_magic(Handle item) override {
        return static_cast<std::uint8_t>(ret(call("IsMagic", H(item), {I(1)})));
    }
    Affect* pc_spell_affect(std::int32_t spa) override {
        return obj<Affect>(call("GetPCSpellAffect", P(&w_.self), {I(spa), I(0), I(0)}));
    }
    void remove_pc_affect(Affect* a, std::uint32_t id) override {
        call("RemovePCAffectex", P(&w_.self), {P(a), I(id)});
    }
    std::int32_t eq_get_time() override { return ret(call("__EQGetTime", std::nullopt, {})); }
    std::int32_t memorized_spell(Handle, std::uint32_t gem) override {
        return ret(call("GetMemorizedSpell", std::nullopt, {I(gem)}));
    }
    std::int32_t spell_focus_pct(const Spell* gem_spell, const Spell* s) override {
        return ret(call("GetSpellFocusPct", P(&w_.self), {P(gem_spell), P(s), I(1), I(0)}));
    }
    void set_spell_recast_timer(Handle pc, std::uint32_t gem) override {
        call("SetSpellRecastTimer", H(pc), {I(gem), I(0)});
    }
    std::uint32_t fast_time() override { return static_cast<std::uint32_t>(ret(call("FastTime", std::nullopt, {}))); }
    void set_linked_reuse(Handle pc, std::uint32_t timer, std::uint32_t when) override {
        call("SetLinkedSpellReuseTimer", H(pc), {I(timer), I(when)});
    }
    std::int32_t random_below(std::uint32_t n) override { return ret(call("0x402870", std::nullopt, {I(n)})); }
    void stop_current_spell_cast() override { call("StopCurrentSpellCast", P(&w_.self), {I(1)}); }
    void apply_fling(Spawn* local, const FlingRecord& r) override {
        call("ApplyFling", P(local), {S()});
        tape("ApplyFling record +0..+0x10", {r.words[0], r.words[1], r.words[2], r.words[3], r.words[4]});
        tape("ApplyFling record +0x14..+0x1c", {r.words[5], r.words[6], r.words[7]});
    }
    std::uint8_t is_valid_teleport(Spawn* s, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t h,
                                   std::uint32_t d) override {
        return static_cast<std::uint8_t>(ret(call("IsValidTeleport", P(s), {I(x), I(y), I(z), I(h), I(d)})));
    }
    std::uint8_t find_buff_level(std::uint32_t id) override {
        return static_cast<std::uint8_t>(ret(call("FindBuffLevelBySpellID", P(&w_.self), {I(id), I(0)})));
    }
    Affect* find_affect_slot(std::uint32_t id, Spawn* caster, std::int32_t* slot, std::uint32_t level,
                             std::uint32_t launch) override {
        const Json* c = call("FindAffectSlot", P(&w_.self), {I(id), P(caster), S(), I(0), I(level), I(0), I(0), B(launch)});
        if (c && (*c)["ret"].type == Json::Type::String) *slot = static_cast<std::int32_t>(w_.answers.affect_slot);
        else if (c) *slot = static_cast<std::int32_t>(w_.answers.affect_slot);
        return obj<Affect>(c);
    }
    void set_buff_caster_id(std::uint32_t slot, std::uint32_t id) override {
        call("SetBuffCasterID", P(&w_.cb), {I(slot), I(id)});
    }
    std::uint8_t has_buff_flag(const Spell* s) override {
        return static_cast<std::uint8_t>(ret(call("0x453520", P(&w_.self), {P(s)})));
    }
    void on_new_buff(std::uint32_t slot) override { call("0x45b250", P(&w_.self), {I(slot)}); }
    std::int32_t refused_mez(std::int32_t max, Spawn* caster, const Spell* s) override {
        return ret(call("EmptyFunction_004432a0", P(&w_.self), {I(max), P(caster), P(s)}));
    }
    std::int32_t total_effect(Character* c, std::int32_t spa) override {
        return ret(call("TotalEffect", P(c), {I(spa), I(1), I(0), I(1), I(1)}));
    }
    void root_aa(std::int32_t total) override {
        call("EmptyFunction_00443570", P(&w_.self), {I(0x9c58c4), I(total)});
    }
    void movement_rate(std::int32_t v) override { call("eqspa_movement_rate", P(&w_.self), {I(v)}); }
    std::uint32_t change_form(const Spell* s, std::uint32_t id, std::int32_t form, Affect* a) override {
        return static_cast<std::uint32_t>(
            ret(call("eqspa_change_form", P(&w_.self), {P(s), I(id), I(form), P(a), I(0)})));
    }
    void lose_control_message(Spawn* s) override { call("ShowLoseControlMessage", P(s), {}); }
    void update_all_visibility() override { call("UpdateAllPlayersVisibility", std::nullopt, {}); }
    void set_view_actor(const char* name) override { call("SetViewActorByName", std::nullopt, {T(name)}); }
    void spa87(std::int32_t v) override { call("0x4940b0", std::nullopt, {I(v)}); }
    void spa57(std::int32_t v) override { call("0x432b20", P(&w_.self), {I(v)}); }
    void update_visible_status() override { call("UpdateMyVisibleStatus", P(&w_.self), {I(0)}); }
    std::int32_t random_low_high(std::int32_t lo, std::int32_t hi) override {
        return ret(call("RandomNumberLowHigh", std::nullopt, {I(lo), I(hi)}));
    }
    std::int32_t calc_affect_change_generic(const Spell* s, std::uint32_t id, std::uint32_t index, Affect* a,
                                            std::int32_t base) override {
        return ret(call("CalcAffectChangeGeneric", P(&w_.self), {P(s), I(id), B(index), P(a), I(base), I(1)}));
    }
    void add_physics_effect(Spawn* s, std::uint32_t id, std::uint32_t caster, std::uint32_t slot,
                            std::int32_t v) override {
        call("AddPhysicsEffect", P(s), {I(id), I(caster), I(slot), I(v)});
    }
    void apply_post_effect(const Spell* s, std::uint32_t index) override {
        call("ApplyPostEffect", P(&w_.self), {P(s), I(index), I(1)});
    }
    void spa436() override { call("0x4325e0", P(&w_.self), {I(1)}); }

private:
    double trig() const {
        return x87_mul(fpu_, X87(std::bit_cast<float>(w_.trig[0])), X87(std::bit_cast<float>(w_.trig[1])));
    }
    void fail(const std::string& why) {
        if (error.empty()) error = why;
    }
    // Compares one recorded value with what the client passed.
    bool same(const Json& rec, const Arg& a) const {
        switch (a.kind) {
        case Arg::Skip: return true;
        case Arg::Int:
            return rec.type == Json::Type::Number && unrebase(rec.u32()) == unrebase(a.v);
        case Arg::Byte:
            if (rec.type == Json::Type::String) return rec.text == "world"; // (the player pointer's upper bytes)
            return (rec.u32() & 0xff) == (a.v & 0xff);
        case Arg::Ptr: {
            Named n = names_.of(a.p);
            if (n.name) return rec.type == Json::Type::String && rec.text == *n.name;
            return rec.type == Json::Type::Number && rec.u32() == n.value;
        }
        case Arg::Text: {
            if (a.t == nullptr) return rec.type == Json::Type::Number && rec.u32() == 0;
            if (rec.type == Json::Type::String && rec.text == "world") return names_.in_world(a.t);
            if (rec.type != Json::Type::String || names_.in_world(a.t)) return false;
            auto p = reinterpret_cast<const char*>(&frame), q = reinterpret_cast<const char*>(&frame + 1);
            if (a.t >= p && a.t < q) {
                deferred_.emplace_back(&rec.text, a.t);
                return true;
            }
            return rec.text == a.t;
        }
        }
        return false;
    }
    const Json* call(const char* fn, std::optional<Arg> self, std::initializer_list<Arg> args) {
        if (!error.empty()) return nullptr;
        if (call_ >= calls_.size()) {
            fail(std::string("extra call ") + fn);
            return nullptr;
        }
        const Json& c = *calls_[call_++];
        std::string where = "call " + std::to_string(call_ - 1) + " " + fn;
        if (c["fn"].text != fn) {
            fail(where + ": the original called " + c["fn"].text);
            return nullptr;
        }
        if (self && self->kind == Arg::Ptr && c.has("this")) {
            const Json& t = c["this"];
            Named n = names_.of(self->p);
            bool ok = n.name ? (t.type == Json::Type::String && (t.text == *n.name || t.text == "world"))
                             : (t.type == Json::Type::Number && t.u32() == n.value);
            if (!ok) fail(where + ": this differs");
        }
        int i = 0;
        for (const Arg& a : args) {
            std::string key = "a" + std::to_string(i++);
            if (a.kind == Arg::Skip) continue;
            if (!c.has(key)) {
                fail(where + ": " + key + " missing");
                continue;
            }
            if (!same(c[key], a)) {
                std::string got = a.kind == Arg::Text ? std::string(a.t ? a.t : "(null)")
                                                      : std::to_string(static_cast<std::int32_t>(a.kind == Arg::Ptr ? names_.of(a.p).value : a.v));
                fail(where + ": " + key + " " + got + " recorded " + (c[key].type == Json::Type::String ? c[key].text : c[key].text));
            }
        }
        return &c;
    }
    void tape(const char* op, std::initializer_list<std::uint32_t> args,
              std::initializer_list<std::uint32_t> masks = {}) {
        if (!error.empty()) return;
        if (tape_at_ >= tape_.size()) {
            fail(std::string("extra virtual ") + op);
            return;
        }
        const Json& r = *tape_[tape_at_++];
        std::string where = "virtual " + std::to_string(tape_at_ - 1) + " " + op;
        if (r[0].text != op) {
            fail(where + ": the original made " + r[0].text);
            return;
        }
        std::size_t i = 0;
        auto m = masks.begin();
        for (std::uint32_t a : args) {
            std::uint32_t mask = m != masks.end() ? *m++ : 0xffffffffu;
            std::uint32_t rec = r[i + 1].u32();
            if ((rec & mask) != (a & mask)) {
                char b[160];
                std::snprintf(b, sizeof b, ": argument %zu %08x recorded %08x", i, a, rec);
                fail(where + b);
            }
            ++i;
        }
    }
    static std::int32_t ret(const Json* c) { return c ? c->operator[]("ret").i32() : 0; }
    template <typename T> T* obj(const Json* c) const {
        if (!c) return nullptr;
        const Json& r = (*c)["ret"];
        if (r.type != Json::Type::String) return nullptr;
        return reinterpret_cast<T*>(names_.object(r.text));
    }
    Handle handle(const Json* c) const {
        if (!c) return 0;
        const Json& r = (*c)["ret"];
        if (r.type != Json::Type::String) return r.u32();
        return names_.object(r.text);
    }

    World& w_;
    Names names_;
    Fpu fpu_;
    mutable std::vector<std::pair<const std::string*, const char*>> deferred_;
    std::vector<const Json*> calls_, tape_;
    std::size_t call_ = 0, tape_at_ = 0;
};

// ---------------------------------------------------------------- comparisons

struct Compare {
    std::string error;
    void u(const char* what, std::uint32_t got, std::uint32_t want) {
        if (error.empty() && got != want) {
            char b[200];
            std::snprintf(b, sizeof b, "%s: %08x, recorded %08x", what, got, want);
            error = b;
        }
    }
    void i(const char* what, const Json& rec, std::int64_t got) { u(what, static_cast<std::uint32_t>(got), rec.u32()); }
    void f(const char* what, const Json& rec, std::uint32_t got) { u(what, got, hailstone::test::f32_bits(rec)); }
    void name(const char* what, const Json& rec, const Names& n, const void* p) {
        if (!error.empty()) return;
        Named v = n.of(reinterpret_cast<std::uintptr_t>(p));
        bool ok = v.name ? rec.type == Json::Type::String && rec.text == *v.name
                         : rec.type == Json::Type::Number && rec.u32() == v.value;
        if (!ok) error = std::string(what) + " differs";
    }
    void spawn(const char* what, const Json& j, const Spawn& s) {
        std::string p = what;
        auto key = [&](const char* k) { return (p + "." + k); };
        i(key("type").c_str(), j["type"], s.type);
        i(key("flag_208").c_str(), j["flag_208"], s.flag_208);
        i(key("owner_38c").c_str(), j["owner_38c"], s.owner_38c);
        i(key("spawn_id").c_str(), j["spawn_id"], s.spawn_id);
        f(key("floor_z").c_str(), j["floor_z"], s.floor_z);
        f(key("y").c_str(), j["y"], s.y);
        f(key("x").c_str(), j["x"], s.x);
        f(key("z").c_str(), j["z"], s.z);
        f(key("speed_y").c_str(), j["speed_y"], s.speed_y);
        f(key("speed_x").c_str(), j["speed_x"], s.speed_x);
        f(key("speed_z").c_str(), j["speed_z"], s.speed_z);
        f(key("rotation_speed").c_str(), j["rotation_speed"], s.rotation_speed);
        f(key("heading").c_str(), j["heading"], s.heading);
        f(key("heading_speed_8c").c_str(), j["heading_speed_8c"], s.heading_speed);
        f(key("model_height_138").c_str(), j["model_height_138"], s.model_height);
        i(key("class").c_str(), j["class"], s.class_id);
        i(key("casting_spell").c_str(), j["casting_spell"], s.casting_spell);
        i(key("stand_state_35c").c_str(), j["stand_state_35c"], s.stand_state);
        i(key("flag_428").c_str(), j["flag_428"], s.flag_428);
        u(key("has_character").c_str(), s.character != nullptr, j["has_character"].truthy());
    }
};

// The regenerated world against the record's inputs.
std::string check_inputs(const Json& in, World& w, const Names& names) {
    Compare c;
    const Json& h = in["hit"];
    c.i("hit.caster_id", h["caster_id"], w.hit.caster_id);
    c.i("hit.item_level", h["item_level"], w.hit.item_level);
    c.f("hit.modifier", h["modifier"], w.hit.modifier);
    c.f("hit.push_heading", h["push_heading"], w.hit.push_heading);
    c.i("hit.spell_1b", h["spell_1b"], w.hit.word_1b);
    c.i("hit.song_value_1d", h["song_value_1d"], w.hit.song_value);
    c.i("hit.spell_id", h["spell_id"], w.hit.spell_id);
    c.i("hit.caster_level", h["caster_level"], w.hit.caster_level);
    c.u("launch", in["launch"].is_null(), w.launch_used == nullptr);
    if (w.launch_used) {
        c.i("launch.byte_32", in["launch"]["byte_32"], w.launch.byte_32);
        c.i("launch.item_flag_40", in["launch"]["item_flag_40"], w.launch.item_flag);
    }
    c.i("duration_override", in["duration_override"], w.duration);
    const Json& s = in["spell"];
    const Spell& sp = w.spells[0];
    c.i("spell.id", s["id"], w.spell_ids[0]);
    c.f("spell.range", s["range"], sp.range);
    c.f("spell.push_back", s["push_back"], sp.push_back);
    c.f("spell.push_up", s["push_up"], sp.push_up);
    c.i("spell.duration_formula", s["duration_formula"], sp.duration_formula);
    for (std::size_t k = 0; k < 12; ++k) {
        c.i("spell.attrib", s["attrib"][k], sp.attrib[k]);
        c.i("spell.base", s["base"][k], sp.base[k]);
        c.i("spell.base2", s["base2"][k], sp.base2[k]);
        c.i("spell.max", s["max"][k], sp.max[k]);
    }
    c.i("spell.recourse", s["recourse_178"], sp.recourse_178);
    c.i("spell.timer", s["timer_id_198"], sp.timer_id_198);
    c.i("spell.flag_240", s["flag_240"], sp.flag_240);
    c.i("spell.type", s["spell_type_26b"], sp.spell_type);
    c.i("spell.26c", s["byte_26c"], sp.byte_26c);
    c.i("spell.target_type", s["target_type_26e"], sp.target_type);
    if (c.error.empty() && (s["name"].text != sp.name.data() || s["teleport_zone"].text != sp.teleport_zone.data()))
        c.error = "spell texts differ";
    for (std::size_t k = 1; k < 8; ++k) {
        const Json& o = in["other_spells"][k - 1];
        c.i("other.id", o["id"], w.spell_ids[k]);
        c.i("other.duration_formula", o["duration_formula"], w.spells[k].duration_formula);
        for (std::size_t j = 0; j < 12; ++j) c.i("other.attrib", o["attrib"][j], w.spells[k].attrib[j]);
        c.i("other.recourse", o["recourse_178"], w.spells[k].recourse_178);
        c.i("other.timer", o["timer_id_198"], w.spells[k].timer_id_198);
        c.i("other.flag_240", o["flag_240"], w.spells[k].flag_240);
    }
    c.u("me present", !in["me"].is_null(), w.has_me);
    if (w.has_me) c.spawn("me", in["me"], w.me);
    std::string caster = w.caster_none ? "none" : (w.caster_me ? "me" : "caster");
    if (c.error.empty() && in["caster"].text != caster) c.error = "caster differs";
    c.spawn("caster_spawn", in["caster_spawn"], w.caster);
    const Json& ch = in["character"];
    c.i("cache_valid", ch["cache_valid_1a4"], w.self.cache_valid);
    c.i("aa_cache_valid", ch["aa_cache_valid_1ac"], w.self.aa_cache_valid);
    c.i("stats_dirty", ch["stats_dirty_c"], w.self.stats_dirty);
    c.i("zone_id", ch["zone_id"], w.cb.zone_id);
    c.i("class", ch["class"], w.profile.class_id);
    c.i("weight", ch["weight_31a8"], w.profile.weight);
    c.i("mana", ch["mana_338c"], w.profile.mana);
    c.i("drunkenness", ch["drunkenness"], w.drunkenness);
    for (std::size_t k = 0; k < 10; ++k) {
        const Json& b = in["buffs"][k];
        c.i("buff.type", b["type"], w.buffs[k].type);
        c.i("buff.level", b["level"], w.buffs[k].level);
        c.i("buff.spell_id", b["spell_id"], w.buffs[k].spell_id);
        for (std::size_t j = 0; j < 12; ++j) c.i("buff.slot", b["slot_data"][j], w.buffs[k].slot_data[j]);
    }
    const Json& g = in["globals"];
    c.i("ruleset", g["ruleset_dcee70"], w.globals.ruleset);
    c.i("zone_type", g["zone_type"], w.globals.zone_type);
    c.i("bind_blocked", g["bind_blocked_ddfc69"], w.globals.bind_blocked);
    c.u("connected", g["connected"].truthy(), w.globals.connected);
    c.name("target", g["target"], names, w.globals.target);
    c.name("local_player", g["local_player"], names, w.globals.local_player);
    c.name("controlled_player", g["controlled_player"], names, w.globals.controlled_player);
    return c.error;
}

std::string check_outputs(const Json& out, World& w, std::uint8_t result) {
    Compare c;
    c.i("value", out["value"], result);
    if (w.has_me && out.has("me")) c.spawn("me", out["me"], w.me);
    c.f("vehicle.speed_y", out["vehicle"]["speed_y"], w.vehicle.speed_y);
    c.f("vehicle.speed_x", out["vehicle"]["speed_x"], w.vehicle.speed_x);
    c.f("vehicle.speed_z", out["vehicle"]["speed_z"], w.vehicle.speed_z);
    c.f("mount.rotation_speed", out["mount"]["rotation_speed"], w.mount.rotation_speed);
    c.f("mount.heading", out["mount"]["heading"], w.mount.heading);
    c.f("mount.heading_speed", out["mount"]["heading_speed_8c"], w.mount.heading_speed);
    const Json& a = out["affect"];
    const Affect& af = w.affect;
    c.i("affect.type", a["type"], af.type);
    c.i("affect.level", a["level"], af.level);
    c.i("affect.byte_2", a["byte_2"], af.byte_2);
    c.i("affect.byte_3", a["byte_3"], af.byte_3);
    c.f("affect.modifier", a["modifier"], af.modifier);
    c.i("affect.spell_id", a["spell_id"], af.spell_id);
    c.i("affect.duration", a["duration"], af.duration);
    c.i("affect.caster_10", a["caster_10"], af.caster_10);
    c.i("affect.dword_14", a["dword_14"], af.dword_14);
    c.f("affect.y_18", a["y_18"], af.y_18);
    c.f("affect.x_1c", a["x_1c"], af.x_1c);
    c.f("affect.z_20", a["z_20"], af.z_20);
    c.i("affect.flags_24", a["flags_24"], af.flags_24);
    for (std::size_t j = 0; j < 12; ++j) c.i("affect.slot_data", a["slot_data"][j], af.slot_data[j]);
    for (std::size_t k = 0; k < 10; ++k)
        for (std::size_t j = 0; j < 12; ++j) c.i("buff_slot_data", out["buff_slot_data"][k][j], w.buffs[k].slot_data[j]);
    const Json& ch = out["character"];
    c.i("cache_valid_1a4", ch["cache_valid_1a4"], w.self.cache_valid);
    c.i("aa_cache_valid_1ac", ch["aa_cache_valid_1ac"], w.self.aa_cache_valid);
    c.i("stats_dirty_c", ch["stats_dirty_c"], w.self.stats_dirty);
    c.f("cb.y", ch["y"], w.cb.y);
    c.f("cb.x", ch["x"], w.cb.x);
    c.f("cb.z", ch["z"], w.cb.z);
    c.f("cb.heading", ch["heading"], w.cb.heading);
    c.i("mesmerized_fc", ch["mesmerized_fc"], w.cb.mesmerized);
    c.i("mana_338c", ch["mana_338c"], w.profile.mana);
    c.i("drunkenness", ch["drunkenness"], w.drunkenness);
    c.i("dword_3248", ch["dword_3248"], w.profile.dword_3248);
    c.i("dword_3264", ch["dword_3264"], w.profile.dword_3264);
    for (std::size_t g = 0; g < 12; ++g) c.i("gem_timers", ch["gem_timers"][g], w.timers[g]);
    const Json& g = out["globals"];
    c.i("local_pushed", g["local_pushed_ddfd59"], w.globals.local_pushed);
    c.i("controlled_pushed", g["controlled_pushed_ddfd58"], w.globals.controlled_pushed);
    c.i("save_reason", g["save_reason_ddfd74"], w.globals.save_reason);
    c.i("packet_opcode", g["packet_opcode_dd00e0"], w.globals.packet_opcode);
    c.i("packet_target", g["packet_target_dd00e2"], w.globals.packet_target);
    c.i("send_count", g["send_count_f99584"], w.globals.send_count);
    return c.error;
}

// Runs a record's world through the client's rules against the replay of its calls.
std::string run_world(const Json& r, World& w, std::uint8_t* result) {
    Fpu fpu(r["precision"].text == "x87-24" ? FpuPrecision::Single24 : FpuPrecision::Double53);
    Replay replay(w, r["out"], fpu);
    HitState state;
    state.self = &w.self;
    state.globals = &w.globals;
    state.frame = &replay.frame;
    state.rof2_single_slot_cures = true;
    // GetSpawnByID answers the record's caster; the world's knob says which.
    *result = hit_by_spell(state, w.hit, w.launch_used, w.duration, replay, fpu);
    return replay.finish();
}

// One stream per file; each record's world comes from a stream of its own, seeded by the file's next draw.
void run_file(const char* file, Set set, std::uint32_t seed, Extra extra) {
    static World world; // large; one per run
    Stream file_stream(seed);
    std::uint32_t record = 0;
    hailstone::test::each_vector(file, [&](const Json& r) -> std::string {
        World& w = world;
        Stream s(file_stream.bits());
        draw_record(w, s, set, extra, record++ * 7);
        Names names(w);
        if (std::string why = check_inputs(r["in"], w, names); !why.empty()) return "regenerated input: " + why;
        std::uint8_t result = 0;
        if (std::string why = run_world(r, w, &result); !why.empty()) return why;
        return check_outputs(r["out"], w, result);
    });
}

// The Moves records' worlds again, with each SPA 379 base raised to 2^26: there 2^26 - 2 rounds back to 2^26, and
// the shadow step's distance loop never ended (the original's x87 code would not end either). The run stops at
// the first step that leaves the distance unchanged; the replay's later mismatches are expected and ignored.
int shadow_step_fixed_points() {
    static World world;
    Stream file_stream(0x7e44);
    std::uint32_t record = 0;
    int raised = 0;
    hailstone::test::each_vector("HitBySpell_Moves.jsonl", [&](const Json& r) -> std::string {
        World& w = world;
        Stream s(file_stream.bits());
        draw_record(w, s, Set::Moves, moves_extra, record++ * 7);
        Names names(w);
        if (!check_inputs(r["in"], w, names).empty()) return "";
        bool any = false;
        for (std::size_t i = 0; i < 12; ++i) {
            if (w.spells[0].attrib[i] != 379 || w.spells[0].base[i] <= 5) continue;
            w.spells[0].base[i] = 1 << 26;
            any = true;
        }
        if (!any) return "";
        ++raised;
        std::uint8_t result = 0;
        (void)run_world(r, w, &result);
        return "";
    });
    return raised;
}

} // namespace

TEST_CASE("HitBySpell: a shadow step (SPA 379) whose float distance cannot shrink ends") {
    const int raised = shadow_step_fixed_points();
    // An empty HAILSTONE_VECTORS_DIR gives a directory with no files in it: the file itself decides.
    const auto dir = hailstone::test::vector_dir();
    std::error_code ec;
    if (!dir || !std::filesystem::is_regular_file(*dir / "HitBySpell_Moves.jsonl", ec))
        MESSAGE("no vectors (HAILSTONE_VECTORS_DIR); nothing checked");
    else
        CHECK(raised > 0);
}

TEST_CASE("HitBySpell vectors: validation") { run_file("HitBySpell_Validation.jsonl", Set::Validation, 0x7e41, validation_extra); }
TEST_CASE("HitBySpell vectors: instant effects") { run_file("HitBySpell_Instant.jsonl", Set::Instant, 0x7e42, nullptr); }
TEST_CASE("HitBySpell vectors: items") { run_file("HitBySpell_Items.jsonl", Set::Items, 0x7e43, nullptr); }
TEST_CASE("HitBySpell vectors: moves") { run_file("HitBySpell_Moves.jsonl", Set::Moves, 0x7e44, moves_extra); }
TEST_CASE("HitBySpell vectors: buffs") { run_file("HitBySpell_Buff.jsonl", Set::Buff, 0x7e45, buff_extra); }

// ---------------------------------------------------------------- the client's landing (game/hit_landing.h)

#include "game/hit_landing.h"
#include "game/sine_table.h"
#include "game/spells.h"
#include "game/world_state.h"

namespace {

std::string landing_spell_line(std::initializer_list<std::pair<int, std::string>> set) {
    std::vector<std::string> cols(237, "0");
    for (int i = 1; i <= 8; ++i) cols[static_cast<std::size_t>(i)].clear();
    for (int i = 86; i <= 97; ++i) cols[static_cast<std::size_t>(i)] = "254";
    for (const auto& [c, v] : set) cols[static_cast<std::size_t>(c)] = v;
    std::string line;
    for (std::size_t i = 0; i < cols.size(); ++i) line += (i ? "^" : "") + cols[i];
    return line;
}

} // namespace

TEST_CASE("A spell landing on the player: its push, and SPA 123 immunity") {
    hailstone::game::SpellTable table;
    std::string text = landing_spell_line({{0, "300"}, {1, "Shove"}, {11, "10"}, {12, "2"}, {83, "0"}, {98, "5"}}) +
                       "\n" + landing_spell_line({{0, "301"}, {1, "Immune"}, {20, "5"}, {86, "123"}, {83, "0"}});
    REQUIRE(table.parse(text) == 2);
    hailstone::game::WorldState world;
    world.player_id = 1;
    world.spawns[1].id = 1;
    world.spawns[1].level = 10;
    world.spawns[2].id = 2;
    world.spawns[2].is_npc = true;
    hailstone::game::SpellState state;
    hailstone::game::HitLandingContext ctx;
    ctx.spells = &table;
    ctx.world = &world;
    ctx.spell_state = &state;
    ctx.level = 10;
    // Pushed toward heading 128: SpeedY += cos * 10 (table entry 0), SpeedX += sin * 10, SpeedZ += 2.
    auto r = hailstone::game::land_spell(hailstone::game::hit_record(2, 0, 1.0f, 128.0f, 0, 0, 300, 10), ctx);
    CHECK(r.value == 1);
    CHECK(r.velocity_change.x == hailstone::game::cmath_cos(128.0f) * 10.0f);
    CHECK(r.velocity_change.y == doctest::Approx(10.0f));
    CHECK(r.velocity_change.z == 2.0f);
    // A detrimental SPA 123 with a non-negative change is refused (message 0x30a0).
    r = hailstone::game::land_spell(hailstone::game::hit_record(2, 0, 1.0f, 0.0f, 0, 0, 301, 10), ctx);
    CHECK(r.value == 0);
    CHECK(r.velocity_change == glm::vec3(0.0f));
}

TEST_CASE("A spell landing on the player: SPA 74 feigns death (StopCastAndFeignDeath)") {
    hailstone::game::SpellTable table;
    // Imitate Death's shape: a beneficial self spell whose first slot is SPA 74 (base 100), and a spell without it.
    std::string text = landing_spell_line({{0, "400"}, {1, "Feign"}, {20, "100"}, {86, "74"}, {83, "1"}}) + "\n" +
                       landing_spell_line({{0, "401"}, {1, "Shove"}, {11, "10"}, {83, "0"}, {98, "5"}});
    REQUIRE(table.parse(text) == 2);
    hailstone::game::WorldState world;
    world.player_id = 1;
    world.spawns[1].id = 1;
    world.spawns[1].level = 10;
    hailstone::game::SpellState state;
    hailstone::game::HitLandingContext ctx;
    ctx.spells = &table;
    ctx.world = &world;
    ctx.spell_state = &state;
    ctx.level = 10;
    auto r = hailstone::game::land_spell(hailstone::game::hit_record(1, 0, 1.0f, 0.0f, 0, 0, 400, 10), ctx);
    CHECK(r.feigned);
    r = hailstone::game::land_spell(hailstone::game::hit_record(1, 0, 1.0f, 0.0f, 0, 0, 401, 10), ctx);
    CHECK_FALSE(r.feigned);
}
