#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "assets/eqg_character.h"
#include "assets/model.h"
#include "assets/pfs.h"
#include "assets/zone.h"
#include "game_files.h"
#include "game/collision.h"
#include "game/doors.h"
#include "game/tradeskill.h"
#include "game/mechanics/floor_height.h"
#include "game/swept_sphere.h"
#include "game/object_collision.h"
#include "net/door_packets.h"
#include "net/opcodes.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

// Byte buffers written the way the server's RoF2 translator writes them
// (rof2.cpp ENCODE, rof2_structs.h offsets), independent of the decoders.
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
void putf(std::vector<std::uint8_t>& b, std::size_t at, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    put32(b, at, u);
}
void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void append_f(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    append_u32(out, u);
}
AppPacket packet(Op op, const std::vector<std::uint8_t>& bytes) {
    AppPacket p(to_wire(op));
    for (auto b : bytes) p.data.push_back(std::byte{b});
    return p;
}
std::vector<std::uint8_t> bytes_of(const AppPacket& p) {
    std::vector<std::uint8_t> out;
    for (auto b : p.data) out.push_back(std::to_integer<std::uint8_t>(b));
    return out;
}

// One Door_Struct as ENCODE(OP_SpawnDoor) fills it.
std::vector<std::uint8_t> door_bytes(const char* name, float y, float x, float z, float heading, std::uint32_t incline,
                                     std::uint32_t size, std::uint8_t id, std::uint8_t type, std::uint8_t state,
                                     std::uint8_t invert, std::uint32_t param) {
    std::vector<std::uint8_t> b(100, 0);
    std::memcpy(b.data(), name, std::strlen(name));
    putf(b, 32, y);
    putf(b, 36, x);
    putf(b, 40, z);
    putf(b, 44, heading);
    put32(b, 48, incline);
    put32(b, 52, size);
    b[60] = id;
    b[61] = type;
    b[62] = state;
    b[63] = invert;
    put32(b, 64, param);
    b[80] = 0;
    b[81] = 1; // "Both must be 1 to allow clicking doors"
    b[82] = 0;
    b[83] = 1;
    return b;
}

} // namespace

TEST_CASE("OP_SpawnDoor records decode as the original reads them and re-encode byte for byte") {
    auto a = door_bytes("DOOR1", 100.5f, -20.25f, 3.0f, 128.0f, 0, 100, 7, 5, 0, 0, 0xffffffffu);
    auto b = door_bytes("POKTELE500", -1.0f, 2.0f, -3.0f, 384.0f, 64, 50, 200, 57, 1, 1, 68);
    std::vector<std::uint8_t> both = a;
    both.insert(both.end(), b.begin(), b.end());
    both.push_back(0x55); // a short tail is ignored (size / 100 records)
    auto doors = decode_spawn_doors(packet(Op::SpawnDoor, both));
    REQUIRE(doors);
    REQUIRE(doors->size() == 2);
    const DoorRecord& d = (*doors)[0];
    CHECK(d.name == "DOOR1");
    CHECK(d.y == 100.5f);
    CHECK(d.x == -20.25f);
    CHECK(d.z == 3.0f);
    CHECK(d.heading == 128.0f);
    CHECK(d.size == 100);
    CHECK(d.door_id == 7);
    CHECK(d.open_type == 5);
    CHECK(d.door_param == -1);
    CHECK(d.usable == 1);
    CHECK(d.visible == 1);
    CHECK(d.remain_open == 0);
    const DoorRecord& e = (*doors)[1];
    CHECK(e.name == "POKTELE500");
    CHECK(e.incline == 64);
    CHECK(e.size == 50);
    CHECK(e.door_id == 200);
    CHECK(e.open_type == 57);
    CHECK(e.state_at_spawn == 1);
    CHECK(e.invert_state == 1);
    CHECK(e.door_param == 68);

    std::vector<std::uint8_t> two(both.begin(), both.end() - 1);
    CHECK(bytes_of(encode_spawn_doors(*doors)) == two);
}

TEST_CASE("OP_ClickDoor is the 16 bytes UseSwitch sends") {
    ClickDoor c;
    c.door_id = 42;
    c.pick_skill = 0;
    c.item_id = 0xffffffffu; // nothing on the cursor
    c.player_id = 0x1234;
    AppPacket p = encode(c);
    CHECK(p.opcode == to_wire(Op::ClickDoor));
    std::vector<std::uint8_t> want = {42, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0x34, 0x12, 0, 0};
    CHECK(bytes_of(p) == want);
    auto back = decode_click_door(p);
    REQUIRE(back);
    CHECK(back->door_id == 42);
    CHECK(back->item_id == 0xffffffffu);
    CHECK(back->player_id == 0x1234);
}

TEST_CASE("OP_MoveDoor and OP_ClickObject") {
    auto m = decode_move_door(packet(Op::MoveDoor, {9, 3}));
    REQUIRE(m);
    CHECK(m->door_id == 9);
    CHECK(m->action == 3);
    CHECK(bytes_of(encode(MoveDoor{9, 2})) == std::vector<std::uint8_t>{9, 2});

    ClickObject c{0x01020304, 77};
    CHECK(bytes_of(encode(c)) == std::vector<std::uint8_t>{4, 3, 2, 1, 77, 0, 0, 0});
    auto back = decode_click_object(packet(Op::ClickObject, {4, 3, 2, 1, 0, 0, 0, 0}));
    REQUIRE(back);
    CHECK(back->drop_id == 0x01020304u);
    CHECK(back->player_id == 0);
}

TEST_CASE("OP_GroundSpawn as ENCODE(OP_GroundSpawn) writes it") {
    std::vector<std::uint8_t> b;
    append_u32(b, 1234);                 // drop_id
    for (char ch : std::string("IT63_ACTORDEF")) b.push_back(static_cast<std::uint8_t>(ch));
    b.push_back(0);
    b.push_back(2); b.push_back(0);      // zone_id 2
    b.push_back(0); b.push_back(0);      // instance
    append_u32(b, 1234);                 // drop_id again
    append_u32(b, 0);
    append_f(b, 256.0f);                 // heading
    append_f(b, 0.0f);
    append_f(b, 0.0f);
    append_f(b, 1.0f);                   // size / 100
    append_f(b, 10.0f);                  // y
    append_f(b, -5.0f);                  // x
    append_f(b, 3.5f);                   // z
    append_u32(b, 1);                    // object_type: temporary
    auto o = decode_ground_object(packet(Op::GroundSpawn, b));
    REQUIRE(o);
    CHECK(o->drop_id == 1234);
    CHECK(o->name == "IT63_ACTORDEF");
    CHECK(o->zone_id == 2);
    CHECK(o->heading == 256.0f);
    CHECK(o->size == 1.0f);
    CHECK(o->y == 10.0f);
    CHECK(o->x == -5.0f);
    CHECK(o->z == 3.5f);
    CHECK(o->object_type == 1);
    CHECK(bytes_of(encode(*o)) == b);
    CHECK_FALSE(decode_ground_object(packet(Op::GroundSpawn, {1, 2, 3})));
}

TEST_CASE("OP_ClickObjectAction, the close packet and OP_TradeSkillCombine") {
    std::vector<std::uint8_t> b(92, 0);
    put32(b, 0, 55);
    put32(b, 4, 0xffffffffu);
    put32(b, 8, 0xffffffffu);
    put32(b, 12, 15);  // type
    put32(b, 20, 700); // icon
    std::memcpy(b.data() + 28, "Forge", 5);
    auto a = decode_click_object_action(packet(Op::ClickObjectAction, b));
    REQUIRE(a);
    CHECK(a->drop_id == 55);
    CHECK(a->unknown04 == -1);
    CHECK(a->type == 15);
    CHECK(a->icon == 700);
    CHECK(a->name == "Forge");
    CHECK(bytes_of(encode(*a)) == b);
    CHECK(encode_close_object().data.empty());
    CHECK(encode_close_object().opcode == to_wire(Op::ClickObjectAction));

    TradeSkillCombine c;
    c.container_slot = ItemSlot{kInvWorld, -1, -1, -1, 0, 0};
    auto p = encode(c);
    REQUIRE(p.data.size() == kTradeSkillCombineSize);
    auto v = bytes_of(p);
    CHECK(v[0] == 4); // typeWorld
    CHECK(v[1] == 0);
}

TEST_CASE("recipe packets: favorites, search, replies, details and auto combine (EQEmu's layouts)") {
    auto fav = bytes_of(encode(RecipeFavorites{15, 0, {101, 202}}));
    REQUIRE(fav.size() == kRecipeFavoritesSize);
    CHECK(fav[0] == 15);
    CHECK(fav[8] == 101);
    CHECK(fav[12] == 202);
    CHECK(fav[16] == 0);

    auto q = bytes_of(encode(RecipesSearch{17, 17001, 10, 200, "bread"}));
    REQUIRE(q.size() == kRecipesSearchSize);
    CHECK(q[4] == 0x69); // 17001 = 0x4269
    CHECK(q[8] == 10);
    CHECK(q[12] == 200);
    CHECK(std::string(reinterpret_cast<const char*>(q.data() + 16)) == "bread");
    CHECK(q[73] == 3); // 0x300

    std::vector<std::uint8_t> r(84, 0);
    put32(r, 0, 15);
    put32(r, 8, 3);
    put32(r, 12, 9876);
    put32(r, 16, 35);
    std::memcpy(r.data() + 20, "Bread", 5);
    auto rr = decode_recipe_reply(packet(Op::RecipeReply, r));
    REQUIRE(rr);
    CHECK(rr->object_type == 15);
    CHECK(rr->component_count == 3);
    CHECK(rr->recipe_id == 9876);
    CHECK(rr->trivial == 35);
    CHECK(rr->name == "Bread");
    CHECK(bytes_of(encode(*rr)) == r);

    CHECK(bytes_of(encode_recipe_details_request(9876)) == std::vector<std::uint8_t>{0x94, 0x26, 0, 0});
    // SendTradeskillDetails: the recipe and the items big-endian, the unused entries' 0xffffffff first.
    std::vector<std::uint8_t> d{0, 0, 0x26, 0x94};
    for (int n = 0; n < 8; ++n) d.insert(d.end(), {0xff, 0xff, 0xff, 0xff});
    for (auto [item, icon, name] : {std::tuple{13087u, 700u, "Flour"}, std::tuple{13006u, 701u, "Water Flask"}}) {
        d.insert(d.end(), {std::uint8_t(item >> 24), std::uint8_t(item >> 16), std::uint8_t(item >> 8), std::uint8_t(item)});
        d.insert(d.end(), {std::uint8_t(icon >> 24), std::uint8_t(icon >> 16), std::uint8_t(icon >> 8), std::uint8_t(icon)});
        d.insert(d.end(), name, name + std::strlen(name) + 1);
    }
    auto dd = decode_recipe_details(packet(Op::RecipeDetails, d));
    REQUIRE(dd);
    CHECK(dd->recipe_id == 9876);
    REQUIRE(dd->components.size() == 2);
    CHECK(dd->components[0].item_id == 13087);
    CHECK(dd->components[0].icon == 700);
    CHECK(dd->components[1].name == "Water Flask");
    CHECK(bytes_of(encode(*dd)) == d);

    RecipeAutoCombine ac;
    ac.object_type = 17;
    ac.container_id = 17001;
    ac.container_slot = ItemSlot::possessions(24);
    ac.recipe_id = 9876;
    auto a = bytes_of(encode(ac));
    REQUIRE(a.size() == kRecipeAutoCombineSize);
    CHECK(a[8] == 0);   // possessions
    CHECK(a[12] == 24); // the slot
    CHECK(a[32] == 0x94);
    auto back = decode_recipe_auto_combine(encode(ac));
    REQUIRE(back);
    CHECK(back->container_slot.same(ItemSlot::possessions(24)));
    CHECK(back->recipe_id == 9876);
}

namespace {
void put_be32(std::vector<std::uint8_t>& d, std::uint32_t v) {
    d.insert(d.end(), {std::uint8_t(v >> 24), std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)});
}
void put_component(std::vector<std::uint8_t>& d, std::uint32_t item, std::uint32_t icon, const char* name) {
    put_be32(d, item);
    put_be32(d, icon);
    d.insert(d.end(), name, name + std::strlen(name) + 1);
}
} // namespace

TEST_CASE("OP_RecipeDetails: ten entries are read and a trailing trivial is skipped") {
    // Stock EQEmu before #5100: the recipe, the unused entries, the components, nothing after.
    std::vector<std::uint8_t> old_layout;
    put_be32(old_layout, 4242);
    for (int n = 0; n < 9; ++n) put_be32(old_layout, 0xffffffff);
    put_component(old_layout, 13866, 1086, "Fishing Bait Ingredients");
    // Since #5100 (2026-06): the same, then the recipe's trivial as a big-endian u32.
    auto new_layout = old_layout;
    put_be32(new_layout, 31);

    for (const auto* d : {&old_layout, &new_layout}) {
        auto dd = decode_recipe_details(packet(Op::RecipeDetails, *d));
        REQUIRE(dd);
        CHECK(dd->recipe_id == 4242);
        REQUIRE(dd->components.size() == 1);
        CHECK(dd->components[0].item_id == 13866);
        CHECK(dd->components[0].icon == 1086);
        CHECK(dd->components[0].name == "Fishing Bait Ingredients");
    }
    // A full recipe: ten components, then the trivial, which is not read as an eleventh.
    std::vector<std::uint8_t> full;
    put_be32(full, 7);
    for (std::uint32_t n = 0; n < 10; ++n) put_component(full, 1000 + n, 500, "Item");
    put_be32(full, 102);
    auto df = decode_recipe_details(packet(Op::RecipeDetails, full));
    REQUIRE(df);
    REQUIRE(df->components.size() == 10);
    CHECK(df->components[9].item_id == 1009);

    // The client reads an unused entry wherever it is, not only before the components.
    std::vector<std::uint8_t> mixed;
    put_be32(mixed, 8);
    put_component(mixed, 13087, 700, "Flour");
    put_be32(mixed, 0xffffffff);
    put_component(mixed, 13006, 701, "Water Flask");
    for (int n = 0; n < 7; ++n) put_be32(mixed, 0xffffffff);
    auto dm = decode_recipe_details(packet(Op::RecipeDetails, mixed));
    REQUIRE(dm);
    REQUIRE(dm->components.size() == 2);
    CHECK(dm->components[1].name == "Water Flask");

    // A reply cut short keeps the entries that came; a name cut short fails the decode.
    std::vector<std::uint8_t> short_reply;
    put_be32(short_reply, 9);
    put_component(short_reply, 13087, 700, "Flour");
    auto ds = decode_recipe_details(packet(Op::RecipeDetails, short_reply));
    REQUIRE(ds);
    CHECK(ds->components.size() == 1);
    short_reply.pop_back();
    CHECK_FALSE(decode_recipe_details(packet(Op::RecipeDetails, short_reply)));
}

TEST_CASE("tradeskill window state: combine containers and recipe answers") {
    CHECK_FALSE(game::is_combine_container(8));
    CHECK(game::is_combine_container(9));
    CHECK(game::is_combine_container(17)); // forge
    CHECK_FALSE(game::is_combine_container(0x33));
    CHECK_FALSE(game::is_combine_container(0x35));
    CHECK_FALSE(game::is_combine_container(0x3a));
    game::TradeskillState t;
    t.object_type = 17;
    t.container_id = 0;
    t.add_recipe({17, 0, 2, 101, 30, "Bronze Bar"});
    t.add_recipe({17, 0, 2, 101, 30, "Bronze Bar"}); // once
    t.add_recipe({15, 0, 2, 102, 30, "Bread"});      // another container's
    t.add_recipe({17, 0, 3, 103, 54, "Steel Bar"});
    REQUIRE(t.recipes.size() == 2);
    CHECK(t.selected_recipe() == nullptr);
    t.selected = 103;
    REQUIRE(t.selected_recipe());
    CHECK(t.selected_recipe()->name == "Steel Bar");
}

TEST_CASE("tradeskill window: favourites in the UI ini, and the trivial colours") {
    // CTradeSkillWnd_LoadFavoritesFromIni: none for "***"; tokens on ',', 0x.. in hex; SaveFavoritesToIni writes the
    // places that are set, each with a ','.
    auto f = game::parse_recipe_favorites("***");
    CHECK(std::all_of(f.begin(), f.end(), [](std::uint32_t r) { return r == 0; }));
    // (a "0x.." token starts with '0', so the place is 0 and the text stops there: the hex case never runs)
    f = game::parse_recipe_favorites("101,0x10,7,");
    CHECK(f[0] == 101);
    CHECK(f[1] == 0);
    CHECK(f[2] == 0);
    f = game::parse_recipe_favorites("101,16,7,");
    CHECK(f[0] == 101);
    CHECK(f[1] == 16);
    CHECK(f[2] == 7);
    CHECK(f[3] == 0);
    CHECK(game::format_recipe_favorites(f) == "101,16,7,");
    // Remove leaves the place empty; Add takes the first empty place.
    CHECK(game::remove_recipe_favorite(f, 16));
    CHECK_FALSE(game::remove_recipe_favorite(f, 16));
    CHECK(game::format_recipe_favorites(f) == "101,7,");
    CHECK(game::add_recipe_favorite(f, 55));
    CHECK(f[1] == 55);
    CHECK(game::is_recipe_favorite(f, 55));
    CHECK_FALSE(game::is_recipe_favorite(f, 0));
    // A text starting with '0' fills the place with 0 and takes no token.
    f = game::parse_recipe_favorites("0,5,");
    CHECK(f[0] == 0);
    CHECK(f[499] == 0);
    // trivial - skill: 0 green, 10 light blue, 25 blue, 50 yellow, else red.
    CHECK(game::recipe_trivial_color(20, 20) == 0xff00ff00u);
    CHECK(game::recipe_trivial_color(30, 20) == 0xff8aa3ffu);
    CHECK(game::recipe_trivial_color(45, 20) == 0xff0000ffu);
    CHECK(game::recipe_trivial_color(70, 20) == 0xffffff00u);
    CHECK(game::recipe_trivial_color(71, 20) == 0xffff0000u);
    CHECK(game::recipe_trivial_color(900, 20) == 0xffff0000u);
}

TEST_CASE("heading helpers follow __FixHeading and __HeadingDiff") {
    CHECK(game::fix_heading(520.0f) == 8.0f);
    CHECK(game::fix_heading(-8.0f) == 504.0f);
    CHECK(game::fix_heading(511.0f) == 511.0f);
    CHECK(game::heading_diff(10.0f, 500.0f) == 22.0f);
    CHECK(game::heading_diff(0.0f, 128.0f) == 128.0f);
    CHECK(game::heading_diff(300.0f, 0.0f) == 212.0f);
}

TEST_CASE("a spawned door starts as EQSwitch::ResetSwitchState puts it") {
    game::DoorSpawn s;
    s.type = 0;
    s.state = 1; // open at spawn: it swings open
    auto d = game::make_door(s, 1000);
    CHECK(d.state == game::kSwitchOpening);
    s.state = 3;
    CHECK(game::make_door(s, 1000).state == game::kSwitchClosed);
    s.type = 0x99; // floor plates stay put
    s.state = 1;
    CHECK(game::make_door(s, 1000).state == game::kSwitchOpen);
    s.type = 100; // spinners become type 0x9c with their top speed
    s.state = 0;
    auto spinner = game::make_door(s, 1000);
    CHECK(spinner.type == 0x9c);
    CHECK(spinner.top_speed1 == 3.0f);
    CHECK(spinner.top_speed2 == doctest::Approx(0.025f));
    s.type = 0;
    s.size = 0; // ScaleFactor < 1 -> 100
    CHECK(game::make_door(s, 1000).scale_factor == 100);
}

TEST_CASE("a hinged door swings a quarter turn at 8 per tick and closes back") {
    game::DoorSpawn s;
    s.type = 5; // swings the other way (types 5..7)
    s.heading = 64.0f;
    auto d = game::make_door(s, 0);
    REQUIRE(d.state == game::kSwitchClosed);
    CHECK(game::change_state(d, game::kSwitchOpening, 0));
    game::process_door(d, 1.0f, 20);
    CHECK(d.heading == 56.0f);
    for (int i = 0; i < 20 && d.state == game::kSwitchOpening; ++i) game::process_door(d, 1.0f, 40);
    CHECK(d.state == game::kSwitchOpen);
    CHECK(d.heading == 448.0f); // 64 - 128, fixed
    // The server's close (OP_MoveDoor 3).
    game::change_state(d, game::kSwitchClosing, 100);
    for (int i = 0; i < 20 && d.state == game::kSwitchClosing; ++i) game::process_door(d, 1.0f, 120);
    CHECK(d.state == game::kSwitchClosed);
    CHECK(d.heading == 64.0f);
}

TEST_CASE("an open door closes itself after RepopFrequency") {
    game::DoorSpawn s;
    s.type = 0;
    auto d = game::make_door(s, 0);
    game::change_state(d, game::kSwitchOpening, 0);
    for (int i = 0; i < 20; ++i) game::process_door(d, 1.0f, 20);
    REQUIRE(d.state == game::kSwitchOpen);
    std::uint32_t opened = d.timestamp;
    CHECK(game::repop_frequency(d) == 10000);
    game::process_door(d, 1.0f, opened + 9000);
    CHECK(d.state == game::kSwitchOpen);
    game::process_door(d, 1.0f, opened + 10001); // RepopSwitch: default state 0 -> closing
    CHECK(d.state == game::kSwitchClosing);
}

TEST_CASE("a lift rises its door_param at 0.45 per tick and comes down at 0.45") {
    game::DoorSpawn s;
    s.type = 0x3b; // 59: the Kelethin lifts
    s.z = 5.0f;
    s.param = 68;
    auto d = game::make_door(s, 0);
    game::change_state(d, game::kSwitchOpening, 0);
    game::process_door(d, 10.0f, 200);
    CHECK(d.z == doctest::Approx(9.5f));
    for (int i = 0; i < 200 && d.state == game::kSwitchOpening; ++i) game::process_door(d, 1.0f, 300);
    CHECK(d.state == game::kSwitchOpen);
    CHECK(d.z == 73.0f);
    game::change_state(d, game::kSwitchClosing, 400);
    game::process_door(d, 1.0f, 420);
    CHECK(d.z == doctest::Approx(72.55f));
    for (int i = 0; i < 200 && d.state == game::kSwitchClosing; ++i) game::process_door(d, 1.0f, 500);
    CHECK(d.state == game::kSwitchClosed);
    CHECK(d.z == 5.0f);
}

TEST_CASE("a sliding door moves sideways by SlideSwitchLeftRight's rules") {
    game::DoorSpawn s;
    s.type = 10; // 8 units at 0.35 per tick
    s.heading = 128.0f; // slides along Y
    s.y = 50.0f;
    auto d = game::make_door(s, 0);
    game::change_state(d, game::kSwitchOpening, 0);
    game::process_door(d, 2.0f, 40);
    CHECK(d.y == doctest::Approx(50.7f));
    for (int i = 0; i < 100 && d.state == game::kSwitchOpening; ++i) game::process_door(d, 1.0f, 60);
    CHECK(d.state == game::kSwitchOpen);
    CHECK(d.y == 58.0f);
    CHECK(d.x == 0.0f);
}

TEST_CASE("process_doors skips a frame that came more than 15 ticks late") {
    std::vector<game::Door> doors(1);
    doors[0] = game::make_door(game::DoorSpawn{}, 0);
    game::change_state(doors[0], game::kSwitchOpening, 0);
    game::SwitchClock clock;
    game::process_doors(doors, clock, 0);
    game::process_doors(doors, clock, 800); // 16 ticks (ms * 0.02): skipped
    CHECK(doors[0].heading == 0.0f);
    game::process_doors(doors, clock, 850); // 1 tick
    CHECK(doors[0].heading == 8.0f);
}

TEST_CASE("particle switches keep one effect while active and stop before reactivation") {
    game::DoorSpawn spawn;
    spawn.id = 7;
    spawn.type = 53;
    spawn.state = spawn.default_state = 1;
    spawn.param = 42;
    spawn.y = 10.0f;
    spawn.x = -20.0f;
    spawn.z = 30.0f;
    std::vector<game::Door> doors{game::make_door(spawn, 0)};
    game::SwitchClock clock;
    int starts = 0, stops = 0;
    bool player = true;
    game::DoorParticles particles;
    particles.local_player = [&] { return player; };
    particles.start = [&](game::Door& d, std::int32_t id) {
        CHECK(d.id == 7);
        CHECK(d.world_position() == glm::vec3(10.0f, -20.0f, 30.0f));
        CHECK_FALSE(d.particle);
        CHECK(id == 42);
        ++starts;
        return true;
    };
    particles.stop = [&](game::Door& d) {
        CHECK(d.particle);
        ++stops;
    };
    auto tick = [&](std::uint32_t now) { game::process_doors(doors, clock, now, 0, nullptr, &particles); };

    tick(100);
    CHECK(doors[0].state == 1);
    CHECK(doors[0].default_state == 1);
    CHECK(doors[0].particle);
    tick(150);
    tick(200);
    CHECK(starts == 1);
    CHECK(stops == 0);

    // Shutdown does not need the player who was required to create the effect.
    player = false;
    doors[0].state = doors[0].default_state = 0;
    tick(250);
    CHECK_FALSE(doors[0].particle);
    tick(300);
    CHECK(stops == 1);
    player = true;
    doors[0].state = doors[0].default_state = 1;
    tick(350);
    CHECK(doors[0].particle);
    CHECK(starts == 2);

    // A new packet replaces the running state; the app retires the old effect by its identity.
    CHECK_FALSE(game::make_door(spawn, 400).particle);
}

TEST_CASE("particle switches require a local player and a valid legacy effect index") {
    game::DoorSpawn spawn;
    spawn.type = 53;
    spawn.state = spawn.default_state = 1;
    spawn.param = 42;
    std::vector<game::Door> doors{game::make_door(spawn, 0)};
    game::SwitchClock clock;
    int starts = 0;
    bool player = false, succeeds = false;
    game::DoorParticles particles;
    particles.local_player = [&] { return player; };
    particles.start = [&](game::Door&, std::int32_t) {
        ++starts;
        return succeeds;
    };
    std::uint32_t now = 0;
    auto tick = [&] { game::process_doors(doors, clock, now += 50, 0, nullptr, &particles); };

    tick();
    CHECK(starts == 0);
    CHECK_FALSE(doors[0].particle);
    player = true;
    for (std::int32_t invalid : {-1, 0, 256}) {
        doors[0].param = invalid;
        tick();
        CHECK_FALSE(doors[0].particle);
    }
    CHECK(starts == 0);
    doors[0].param = 255;
    tick();
    CHECK(starts == 1);
    CHECK_FALSE(doors[0].particle);
    succeeds = true;
    tick();
    CHECK(starts == 2);
    CHECK(doors[0].particle);
    tick();
    CHECK(starts == 2);
}

TEST_CASE("particle switch day and night callbacks run before the active state is tested") {
    game::DoorSpawn spawn;
    spawn.type = 53;
    spawn.param = 1;
    std::vector<game::Door> doors{game::make_door(spawn, 0)};
    game::SwitchClock clock;
    int starts = 0, stops = 0;
    bool active = true;
    game::DoorParticles particles;
    particles.local_player = [] { return true; };
    particles.update_day_night = [&](game::Door& d) { d.state = d.default_state = active ? 1 : 0; };
    particles.start = [&](game::Door&, std::int32_t) { ++starts; return true; };
    particles.stop = [&](game::Door&) { ++stops; };

    game::process_doors(doors, clock, 100, 0, nullptr, &particles);
    CHECK(starts == 1);
    CHECK(doors[0].particle);
    active = false;
    game::process_doors(doors, clock, 150, 0, nullptr, &particles);
    CHECK(stops == 1);
    CHECK_FALSE(doors[0].particle);
}

TEST_CASE("particle switch day and night modes use native hours and preserve packet-controlled states") {
    game::Door d;
    d.type = 53;
    d.default_state = game::kSwitchOpen;
    d.timestamp = 123;
    for (float mode : {0.0f, 3.0f}) {
        d.default_door_angle = mode;
        d.state = game::kSwitchOpening;
        game::update_particle_switch_day_night(d, 12, true);
        CHECK(d.state == game::kSwitchOpening);
    }
    for (const auto& [hour, day] : {std::pair{1, false}, std::pair{4, false}, std::pair{5, true},
                                  std::pair{18, true}, std::pair{19, false}, std::pair{24, false}}) {
        d.default_door_angle = 1.0f;
        game::update_particle_switch_day_night(d, hour);
        CHECK(d.state == (day ? game::kSwitchOpen : game::kSwitchClosed));
        game::update_particle_switch_day_night(d, hour, true);
        CHECK(d.state == (day ? game::kSwitchOpen : game::kSwitchClosed));
        d.default_door_angle = 2.0f;
        game::update_particle_switch_day_night(d, hour);
        CHECK(d.state == (day ? game::kSwitchClosed : game::kSwitchOpen));
        game::update_particle_switch_day_night(d, hour, true);
        CHECK(d.state == game::kSwitchOpen);
    }
    CHECK(d.default_state == game::kSwitchOpen);
    CHECK(d.timestamp == 123);
    d.type = 0;
    d.state = game::kSwitchClosed;
    game::update_particle_switch_day_night(d, 1, true);
    CHECK(d.state == game::kSwitchClosed);
}

TEST_CASE("a type 0x28 door off its default state is clicked back once per repop period, at any frame rate") {
    game::DoorSpawn s;
    s.type = 0x28;
    s.default_state = game::kSwitchClosed;
    std::vector<game::Door> doors{game::make_door(s, 0)};
    doors[0].state = game::kSwitchOpen; // at rest open while its default is closed
    REQUIRE(game::repop_frequency(doors[0]) == 20000);
    game::SwitchClock clock;
    int clicks = 0;
    // 240 frames a second for 1.5 s past the repop time, the server's OP_MoveDoor not yet back: one click, not one a
    // frame (EQEmu toggles the door on each).
    for (std::uint32_t ms = 19000; ms < 21500; ms += 4) clicks += static_cast<int>(game::process_doors(doors, clock, ms).size());
    CHECK(clicks == 1);
    CHECK(doors[0].state == game::kSwitchOpen); // the state is the server's to change
    // No answer (a locked door, a lost packet): clicked again a repop period later.
    for (std::uint32_t ms = 21500; ms < 41500; ms += 4) clicks += static_cast<int>(game::process_doors(doors, clock, ms).size());
    CHECK(clicks == 2);
}

TEST_CASE("door transforms put the actor at (Y, X, Z), turned by heading, scaled") {
    game::Door d;
    d.y = 10.0f;
    d.x = 20.0f;
    d.z = 30.0f;
    d.heading = 128.0f; // a quarter turn counterclockwise: +X -> +Y
    d.scale_factor = 50;
    glm::mat4 m = game::door_transform(d);
    glm::vec3 origin = glm::vec3(m * glm::vec4(0, 0, 0, 1));
    CHECK(origin.x == doctest::Approx(10.0f));
    CHECK(origin.y == doctest::Approx(20.0f));
    CHECK(origin.z == doctest::Approx(30.0f));
    glm::vec3 front = glm::vec3(m * glm::vec4(2, 0, 0, 1)) - origin;
    CHECK(front.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(front.y == doctest::Approx(1.0f));
    // A door angle of 128 tilts the actor a quarter turn about its Y axis.
    d.heading = 0.0f;
    d.scale_factor = 100;
    d.door_angle = 128.0f;
    glm::vec3 up = glm::vec3(game::door_transform(d) * glm::vec4(1, 0, 0, 1)) - d.world_position();
    CHECK(std::abs(up.z) == doctest::Approx(1.0f));
}

TEST_CASE("door transforms are SetEulerRotation512's matrix") {
    // CDisplay::CreateActor passes (heading, door angle, 0) to the actor's vf74; both actor classes call
    // CActorData::SetEulerRotation512 @0x100c4200 (the hierarchical one adds its +0x21c / +0x220, 0 here), which
    // stores the columns (cos a cos B, sin a cos B, -sin B), ... with B = -(door angle): Rz(a) Ry(-b).
    game::Door d;
    d.heading = 70.0f;
    d.door_angle = 40.0f;
    glm::mat4 m = game::door_transform(d);
    const float k = 2.0f * 3.14159265f / 512.0f;
    float a = d.heading * k, b = -d.door_angle * k;
    glm::vec3 col0(std::cos(a) * std::cos(b), std::sin(a) * std::cos(b), -std::sin(b));
    glm::vec3 col2(std::cos(a) * std::sin(b), std::sin(a) * std::sin(b), std::cos(b));
    for (int i = 0; i < 3; ++i) {
        CHECK(m[0][i] == doctest::Approx(col0[i]).epsilon(1e-5));
        CHECK(m[2][i] == doctest::Approx(col2[i]).epsilon(1e-5));
    }
}

TEST_CASE("which switches block movement") {
    // CollisionFilter_Movement @0x4760c0 drops types 0x32 and 0x33 only; IsInvisible is not asked on the sweep's
    // path, so the invisible types (0x36, 0x39 with a door angle of 1) block; 0x35 has no actor.
    game::Door d;
    for (std::uint8_t t : {std::uint8_t{0}, std::uint8_t{0x3b}, std::uint8_t{0x36}, std::uint8_t{0x39}}) {
        d.type = t;
        d.default_door_angle = 1.0f;
        CHECK(game::door_blocks_movement(d));
    }
    for (std::uint8_t t : {std::uint8_t{0x32}, std::uint8_t{0x33}, std::uint8_t{0x35}}) {
        d.type = t;
        CHECK_FALSE(game::door_blocks_movement(d));
    }
    d.type = 0x36;
    CHECK(game::door_is_invisible(d));
}

TEST_CASE("harbingers' BCN door is an EQGS model with drawable pieces") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    auto path = game->find("harbingers.eqg");
    if (!path) return;
    auto archive = assets::PfsArchive::open(*path);
    CHECK_FALSE(archive.read("bcn.mod"));
    auto data = archive.read("bcn.mds");
    REQUIRE(data);
    auto mesh = assets::eqg_character::parse_mesh(*data, "BCN");
    CHECK(mesh.mds);
    assets::CharacterMesh c;
    assets::eqg_character::append_pieces(mesh, assets::eqg_character::select_pieces(mesh, "BCN", 0, 0), c);
    // Three variants (OBJ_beacon0200, 0300, 0400); select_pieces falls back to the first.
    CHECK(mesh.pieces.size() == 3);
    CHECK(c.indices.size() >= 3);
}

TEST_CASE("UseSwitch's reach and the switch rules") {
    game::Door d;
    d.y = 0;
    d.x = 0;
    d.z = 0;
    CHECK(game::door_within_reach(d, {30, 0, 0}, 6.0f, 5.0f));
    CHECK_FALSE(game::door_within_reach(d, {32, 0, 0}, 6.0f, 5.0f));
    d.type = 0x36;
    CHECK_FALSE(game::door_can_be_used(d));
    d.type = 0;
    d.state = game::kSwitchOpening;
    CHECK_FALSE(game::door_can_be_used(d)); // still moving
    d.state = game::kSwitchOpen;
    CHECK(game::door_can_be_used(d));
    d.type = 0x1e;
    CHECK(game::is_floor_switch(d, 2));
    d.type = 0;
    CHECK_FALSE(game::is_floor_switch(d, 2));
    d.type = 0x1a;
    CHECK(game::is_floor_switch(d, 0xcd));
}

namespace {

// An axis-aligned box as a mesh (12 triangles).
assets::Mesh box_mesh(glm::vec3 lo, glm::vec3 hi) {
    assets::Mesh m;
    for (int i = 0; i < 8; ++i) {
        assets::Vertex v;
        v.position = {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
        m.vertices.push_back(v);
    }
    const std::uint32_t faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (const auto& f : faces) m.indices.insert(m.indices.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
    m.materials.push_back(assets::Material{});
    m.parts.push_back({0, static_cast<std::uint32_t>(m.indices.size()), 0, true});
    return m;
}

} // namespace

TEST_CASE("a closed door blocks the doorway; swung open it does not") {
    // A slab hinged at its origin: 10 wide along +Y, 1 thick, 12 high.
    auto shape = game::ObjectShape::from_mesh(box_mesh({-0.5f, 0.0f, 0.0f}, {0.5f, 10.0f, 12.0f}));
    REQUIRE(shape.tris.size() == 36);
    game::Door d;
    d.y = 100.0f; // world x
    d.x = 200.0f; // world y
    glm::mat4 closed = game::door_transform(d);
    glm::vec3 a(95.0f, 205.0f, 5.0f), b(105.0f, 205.0f, 5.0f); // through the middle of the doorway
    CHECK(game::blocks(shape, closed, glm::inverse(closed), 1.0f, a, b));
    d.heading = 128.0f; // type 0 opens by +128
    glm::mat4 open = game::door_transform(d);
    CHECK_FALSE(game::blocks(shape, open, glm::inverse(open), 1.0f, a, b));
    // Open, it lies along -X from the hinge instead.
    CHECK(game::blocks(shape, open, glm::inverse(open), 1.0f, {95.0f, 195.0f, 5.0f}, {95.0f, 205.0f, 5.0f}));
}

TEST_CASE("a mesh part that reaches past its index list is read up to the list's end") {
    auto m = box_mesh({0, 0, 0}, {1, 1, 1});
    m.parts[0].index_count += 300; // a damaged count
    auto shape = game::ObjectShape::from_mesh(m);
    CHECK(shape.tris.size() == 36);
    m.parts[0].first_index = 0xfffffff0u; // and a start past the end
    m.parts[0].index_count = 0x20u;
    CHECK(game::ObjectShape::from_mesh(m).tris.empty());
    assets::Zone z;
    z.terrain.push_back(m);
    CHECK(game::CollisionMesh::build(z).triangle_count() == 0);
}

TEST_CASE("a lift is a floor") {
    auto shape = game::ObjectShape::from_mesh(box_mesh({-5.0f, -5.0f, -1.0f}, {5.0f, 5.0f, 0.0f}));
    game::Door lift;
    lift.type = 0x3b;
    lift.y = 10.0f;
    lift.x = 20.0f;
    lift.z = 5.0f;
    glm::mat4 at = game::door_transform(lift);
    auto floor = game::floor_under(shape, glm::inverse(at), {12.0f, 21.0f, 5.5f}, 3.0f);
    REQUIRE(floor);
    CHECK(*floor == doctest::Approx(5.0f));
    CHECK_FALSE(game::floor_under(shape, glm::inverse(at), {30.0f, 21.0f, 5.5f}, 3.0f)); // off the platform
}

TEST_CASE("a moving floor switch resets its riders' floor cache; other lifts do not") {
    // ProcessSwitches (game/mechanics/switches.h): a rider within its actor radius + the switch's + 20 of where
    // the switch was gets CollisionCounter = 0; type 0x3b (the elevator) is a floor switch, 0x3c is not.
    for (std::uint8_t type : {std::uint8_t{0x3b}, std::uint8_t{0x3c}}) {
        game::Door lift;
        lift.type = type;
        lift.y = lift.default_y = 10.0f;
        lift.x = lift.default_x = 20.0f;
        lift.z = lift.default_z = 5.0f;
        lift.param = 40;
        lift.state = game::kSwitchOpening;
        std::vector<game::Door> doors{lift};
        game::SwitchClock clock;
        game::mechanics::SwitchSpawn near{12.0f, 21.0f, 1.78}, far{200.0f, 21.0f, 1.78};
        std::vector<game::mechanics::SwitchSpawn> spawns{near, far};
        std::vector<int> reset;
        game::DoorRiders riders{spawns, [&](std::size_t i) { reset.push_back(static_cast<int>(i)); },
                                [](const game::Door&) { return 8.0; }};
        game::process_doors(doors, clock, 1000, 0, &riders);
        game::process_doors(doors, clock, 1050, 0, &riders); // one tick
        CHECK(doors[0].z > 5.0f); // rising (0.45 per tick for 0x3b, 0.45 for 0x3c)
        // Every frame it moves (both here), only the near spawn.
        if (type == 0x3b) CHECK(reset == std::vector<int>{0, 0});
        else CHECK(reset.empty());
    }
}

TEST_CASE("qeynos DOOR1 (door 59 on the NMS server) blocks its doorway closed and clears it open") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    auto zone = assets::load_zone(*game, "qeynos");
    auto it = zone.objects.find("DOOR1_ACTORDEF");
    REQUIRE(it != zone.objects.end());
    auto shape = game::ObjectShape::from_mesh(it->second);
    MESSAGE("DOOR1: " << shape.tris.size() / 3 << " solid triangles, bounds (" << shape.lo.x << " " << shape.lo.y << " "
                      << shape.lo.z << ") - (" << shape.hi.x << " " << shape.hi.y << " " << shape.hi.z << ")");
    REQUIRE_FALSE(shape.empty());
    game::DoorSpawn s;
    s.name = "DOOR1";
    s.y = 30.1f;
    s.x = -560.2f;
    s.z = 28.0f;
    s.heading = 128.0f;
    auto d = game::make_door(s, 0);
    glm::mat4 closed = game::door_transform(d);
    glm::vec3 c = glm::vec3(closed * glm::vec4((shape.lo + shape.hi) * 0.5f, 1.0f));
    // Across the door (along world y at heading 128) at waist height.
    glm::vec3 a(c.x, c.y + 6.0f, 28.0f + 3.5f), b(c.x, c.y - 6.0f, 28.0f + 3.5f);
    CHECK(game::blocks(shape, closed, glm::inverse(closed), 1.0f, a, b));
}

namespace {
// The floor test (GetPlayerFloorHeight) over a single placed door shape.
struct LiftFloor final : game::mechanics::FloorQueries {
    const game::ObjectShape& shape;
    const game::Door& door;
    LiftFloor(const game::ObjectShape& s, const game::Door& d) : shape(s), door(d) {}
    bool is_3d() override { return true; }
    bool has_vehicle() override { return false; }
    bool scene_accepts(const std::array<float, 3>&) override { return true; }
    void set_vehicle_none() override {}
    std::uint32_t sweep(const std::array<float, 3>& a, const std::array<float, 3>& b, float r, bool,
                        float& hit_z) override {
        glm::mat4 t = game::door_transform(door);
        game::SphereSweep s = game::SphereSweep::make({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, r);
        game::sweep_sphere(shape, t, glm::inverse(t), door.scale(), s);
        hit_z = game::sweep_output(s).position.z;
        return 0;
    }
};

// A platform's top, wound as WLD files are (clockwise from the front, so its record normal faces up).
assets::Mesh platform_top(float half) {
    assets::Mesh m;
    for (auto [x, y] : {std::pair{-half, -half}, {-half, half}, {half, half}, {half, -half}}) {
        assets::Vertex v;
        v.position = {x, y, 0.0f};
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    m.materials.push_back(assets::Material{});
    m.parts.push_back({0, 6, 0, true});
    return m;
}

// A rider standing still on a lift for `frames` frames: ProcessSwitches, DoCollisionMovement's counter, the
// floor test, the landing. Returns the feet's z.
float ride(std::uint8_t type, int frames) {
    auto shape = game::ObjectShape::from_mesh(platform_top(5.0f));
    game::Door lift;
    lift.type = type;
    lift.y = lift.default_y = 10.0f;
    lift.x = lift.default_x = 20.0f;
    lift.z = lift.default_z = 5.0f;
    lift.param = 40;
    lift.state = game::kSwitchOpening;
    std::vector<game::Door> doors{lift};
    game::SwitchClock clock;
    game::mechanics::FloorCache cache;
    cache.ceiling = game::mechanics::kNoFloorHeight;
    game::mechanics::FloorWorld world;
    float feet = 5.0f, avatar = 3.75f;
    std::vector<game::mechanics::SwitchSpawn> spawns(1);
    game::DoorRiders riders{spawns, [&](std::size_t) { cache.collision_counter = 0; },
                            [](const game::Door&) { return 8.0; }};
    for (int i = 0; i < frames; ++i) {
        spawns[0] = {12.0f, 21.0f, 1.78};
        game::process_doors(doors, clock, 1000u + 50u * static_cast<std::uint32_t>(i), 0, &riders);
        ++cache.collision_counter; // DoCollisionMovement counts each move
        LiftFloor q(shape, doors[0]);
        float floor = static_cast<float>(game::mechanics::get_player_floor_height(cache, {12.0f, 21.0f, feet + avatar}, true,
                                                                                  avatar, world, q));
        if (feet < floor) feet = floor; // the landing check
    }
    return feet;
}
} // namespace

TEST_CASE("a rising elevator (a floor switch) lifts its rider through the floor test, not a carry") {
    // Type 0x3b rises 0.45 per tick: after 20 one-tick frames about 9 units; the rider's cache is reset each frame
    // and its floor follows the top. Type 0x3c (not a floor switch) rises too, but a rider standing still keeps its
    // cached floor (the original's behaviour).
    float up = ride(0x3b, 20);
    CHECK(up > 12.0f);
    float stale = ride(0x3c, 20);
    CHECK(stale < 6.0f);
}

TEST_CASE("CDisplay::CreateActor's definition name: _ACTORDEF only on names under 10 characters") {
    using hailstone::game::create_actor_definition;
    CHECK(create_actor_definition("IT63") == "IT63_ACTORDEF");
    CHECK(create_actor_definition("IT") == "IT_ACTORDEF");
    CHECK(create_actor_definition("IT10804_ACTORDEF") == "IT10804_ACTORDEF");
    CHECK(create_actor_definition("OBJ_TREE_B") == "OBJ_TREE_B"); // 10 characters: as it is
    CHECK(create_actor_definition("player_1") == "player_1");
    CHECK(create_actor_definition(std::string(80, 'A')).size() == 0x3f);
}

namespace {

// A closed hinged door fits its doorway: the slab itself and the way through it, at three heights and three points
// across, are clear of the zone's geometry.
bool fills_doorway(const game::CollisionMesh& zone, const game::ObjectShape& shape, const game::Door& d) {
    glm::mat4 t = game::door_transform(d);
    glm::vec3 ext = shape.hi - shape.lo;
    bool thin_x = ext.x < ext.y;
    glm::vec3 c = glm::vec3(t * glm::vec4((shape.lo + shape.hi) * 0.5f, 1.0f));
    glm::vec3 n = glm::normalize(glm::vec3(t * glm::vec4(thin_x ? 1 : 0, thin_x ? 0 : 1, 0, 0)));
    glm::vec3 w = glm::normalize(glm::vec3(t * glm::vec4(thin_x ? 0 : 1, thin_x ? 1 : 0, 0, 0)));
    float half = (thin_x ? ext.y : ext.x) * d.scale() * 0.5f;
    for (float zf : {-0.25f, 0.0f, 0.25f}) {
        glm::vec3 cz = c + glm::vec3(0, 0, zf * ext.z * d.scale());
        if (!zone.line_of_sight(cz - w * (half - 0.5f), cz + w * (half - 0.5f))) return false;
        for (float wf : {-0.6f, 0.0f, 0.6f})
            if (!zone.line_of_sight(cz + w * half * wf - n * 4.0f, cz + w * half * wf + n * 4.0f)) return false;
    }
    return true;
}

} // namespace

TEST_CASE("closed doors fill their doorways (NMS server doors in four zones)") {
    // Over the PEQ doors of 21 classic zones (hinged types 0..9), the transform fits 655 of 665 closed doors
    // in their doorways; heading turned the other way (512 - h, a clockwise Rz) fits 313. These doors fit only
    // the way the client turns them. DB rows: pos_x, pos_y, pos_z, heading (the record's Y is pos_y).
    struct Row {
        const char* zone;
        const char* model;
        float pos_x, pos_y, pos_z, heading;
    };
    const Row rows[] = {
        {"felwithea", "DOOR1", -320.231f, -123.909f, 0.00097f, 128.0f},
        {"felwithea", "DOOR1", -363.372f, 217.888f, 0.00097f, 384.0f},
        {"freporte", "DOOR1", -230.69f, 32.028f, -55.9766f, 128.0f},
        {"freporte", "DOOR1", -476.259f, -84.0134f, -83.9654f, 384.0f},
        {"kaladima", "VUDOOR", -165.825f, 168.027f, 0.001f, 128.0f},
        {"kaladima", "VUDOOR", -339.557f, 1.99755f, 7.9978f, 384.0f},
        {"qeynos2", "DOOR1", -41.1786f, 137.933f, 0.001f, 384.0f},
    };
    const GameDir* game = test::game_dir();
    if (!game) return;
    std::string loaded;
    assets::Zone zone;
    std::optional<game::CollisionMesh> mesh;
    for (const Row& r : rows) {
        CAPTURE(r.zone);
        CAPTURE(r.heading);
        if (loaded != r.zone) {
            zone = assets::load_zone(*game, r.zone);
            mesh = game::CollisionMesh::build(zone);
            loaded = r.zone;
        }
        auto it = zone.objects.find(std::string(r.model) + "_ACTORDEF");
        REQUIRE(it != zone.objects.end());
        auto shape = game::ObjectShape::from_mesh(it->second);
        game::DoorSpawn s;
        s.name = r.model;
        s.y = r.pos_y;
        s.x = r.pos_x;
        s.z = r.pos_z;
        s.heading = r.heading;
        game::Door d = game::make_door(s, 0);
        CHECK(fills_doorway(*mesh, shape, d));
        game::Door clockwise = d; // Rz(-h): for these headings the slab swings out of the doorway around the hinge
        clockwise.heading = game::fix_heading(512.0f - r.heading);
        CHECK_FALSE(fills_doorway(*mesh, shape, clockwise));
    }
}
