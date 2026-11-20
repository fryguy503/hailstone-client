// The merchant's "Show only items I can use" (MW_UsableButton; CMerchantWnd::PurchasePageHandler::UpdateList @0x6f0bb0
// with +0x60 clear) over the hs API: on to start, what the character's class or race cannot use and spell scrolls it
// knows (or knows a higher rank of) are left out of ItemList, and a click shows everything.
#include <doctest/doctest.h>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/byte_io.h"
#include "game/mechanics/combat_ability.h"
#include "game/spells.h"
#include "mq_test_world.h"
#include "net/item_packets.h"
#include "net/merchant_packets.h"
#include "net/packet_io.h"

using namespace hailstone;

namespace {

struct Lua {
    lua_State* L = luaL_newstate();
    explicit Lua(app::UiApi& api) {
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
    }
    ~Lua() { lua_close(L); }
    std::string text(const char* code) {
        REQUIRE(luaL_dostring(L, code) == LUA_OK);
        const char* s = lua_tostring(L, -1);
        std::string v = s ? s : "";
        lua_settop(L, 0);
        return v;
    }
};

// The item names in the merchant's list, joined by ",".
constexpr const char* kNames = R"(
    local out = {}
    for r = 0, (hs.ListInfo('MerchantWnd', 'ItemList') or 0) - 1 do out[#out + 1] = hs.ListRow('MerchantWnd', 'ItemList', r)[2] end
    return table.concat(out, ','))";

net::ItemPacket goods(std::uint32_t slot, const char* name) {
    net::ItemPacket ip;
    ip.type = net::kItemPacketMerchant;
    ip.item.id = 1000 + slot;
    ip.item.name = name;
    ip.item.price = 10;
    ip.item.slot.slot = static_cast<std::int16_t>(slot);
    ip.item.classes = 0xffff;
    ip.item.races = 0xffff;
    return ip;
}

void open_merchant(net::GameConnection& c) {
    auto& world = c.world_for_test();
    game::Spawn m = *world.player();
    m.id = 5001;
    m.name = "Merchant00";
    m.class_id = 41;
    m.is_npc = true;
    world.spawns[m.id] = m;
    c.open_merchant(m.id);
    net::MerchantClick open;
    open.npc_id = m.id;
    open.player_id = world.player_id;
    open.rate = 1.0f;
    c.receive_zone_packet_for_test(net::encode(open));
    REQUIRE(c.merchant()->open);
}

} // namespace

TEST_CASE("merchant: Show only items I can use leaves out what the character cannot use, and is on to start") {
    ImGuiContext* ctx = ImGui::CreateContext(); // the hs API reads the keyboard's modifiers
    mq_test::World w("hailstone_merchant_usable_test"); // Tester: a human cleric
    auto& c = *w.conn;

    // Healing ranks 1..3 (group 900, subgroup 1); the book holds rank 3.
    auto table = std::make_shared<game::SpellTable>();
    std::string text;
    for (int r = 1; r <= 3; ++r)
        text += mq_test::spell_line({{0, std::to_string(800 + r)}, {1, "Healing Rk. " + std::to_string(r)},
                                     {207, "900"}, {208, std::to_string(r)}, {236, "1"}}) + "\n";
    text += mq_test::spell_line({{0, "810"}, {1, "Shielding"}}) + "\n";
    REQUIRE(table->parse(text) == 4);
    c.set_spell_table_for_test(table);
    net::GameConnectionTestAccess::spells(c).book[0] = 803;

    open_merchant(c);

    auto potion = goods(1, "Potion");
    auto plate = goods(2, "Warrior Plate");
    plate.item.classes = 1u << 0; // warriors only
    auto hat = goods(3, "Ogre Hat");
    hat.item.races = 1u << 9; // ogres only
    auto bag = goods(4, "Satchel");
    bag.item.item_class = 1;
    bag.item.classes = bag.item.races = 0; // a bag stays whatever its masks say
    auto rk2 = goods(5, "Spell: Healing Rk. 2");
    rk2.item.item_type = 20;
    rk2.item.effects[net::kEffectScroll].effect = 802; // rank 3 is known
    auto shield = goods(6, "Spell: Shielding");
    shield.item.item_type = 20;
    shield.item.effects[net::kEffectScroll].effect = 810;
    for (const auto* ip : {&potion, &plate, &hat, &bag, &rk2, &shield})
        c.receive_zone_packet_for_test(net::encode_item_packet(false, *ip));
    REQUIRE(c.merchant()->items.size() == 6);

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &c;
    app::UiApi api(host);
    Lua lua(api);

    CHECK(lua.text("return tostring(select(2, hs.ChildState('MerchantWnd', 'MW_UsableButton')))") == "true");
    CHECK(lua.text(kNames) == "Potion,Satchel,Spell: Shielding");

    // A row picked while filtered is still the one bought: the rows map to the merchant's slots.
    lua.text("hs.ListClick('MerchantWnd', 'ItemList', 2, 'left')");
    CHECK(lua.text("return (hs.WindowText('MerchantWnd', 'MW_SelectedItemLabel'))") == "Spell: Shielding");

    // Once known, the scroll goes too.
    net::GameConnectionTestAccess::spells(c).book[1] = 810;
    api.begin_frame();
    CHECK(lua.text(kNames) == "Potion,Satchel");

    // A multiclass server can report Bard in the profile, but the merchant must use the actual classes.
    auto prof = *c.profile();
    prof.class_ = 8; // Bard
    prof.level = 1;
    c.set_profile_for_test(prof);
    constexpr auto mage = 1u << (13 - 1);
    constexpr auto enc = 1u << (14 - 1);
    constexpr auto nec = 1u << (11 - 1);
    auto mage_scroll = goods(7, "Spell: Mage Upgrade");
    mage_scroll.item.classes = mage;
    mage_scroll.item.item_type = 20;
    mage_scroll.item.effects[net::kEffectScroll].effect = 811;
    text += mq_test::spell_line({{0, "811"}, {1, "Mage Upgrade"}, {116, "50"}}) + "\n";
    auto multiclass_table = std::make_shared<game::SpellTable>();
    REQUIRE(multiclass_table->parse(text) == 5);
    REQUIRE(multiclass_table->get(811));
    REQUIRE(multiclass_table->get(811)->class_level[12] == 50);
    REQUIRE(prof.level < 50);
    c.set_spell_table_for_test(multiclass_table);
    c.receive_zone_packet_for_test(net::encode_item_packet(false, mage_scroll));
    api.begin_frame();
    CHECK(lua.text(kNames) == "Potion,Satchel");
    c.set_player_classes(mage | enc | nec);
    api.begin_frame();
    CHECK(lua.text(kNames) == "Potion,Satchel,Spell: Mage Upgrade");
    net::GameConnectionTestAccess::spells(c).book[2] = 811;
    api.begin_frame();
    CHECK(lua.text(kNames) == "Potion,Satchel");

    lua.text("hs.ClickChild('MerchantWnd', 'MW_UsableButton', 'left')");
    CHECK(lua.text("return tostring(select(2, hs.ChildState('MerchantWnd', 'MW_UsableButton')))") == "false");
    CHECK(lua.text(kNames) ==
          "Potion,Warrior Plate,Ogre Hat,Satchel,Spell: Healing Rk. 2,Spell: Shielding,Spell: Mage Upgrade");

    // The checkbox belongs to the purchase page.
    lua.text("hs.SetTabPage('MerchantWnd', 'MW_MerchantSubwindows', 1)");
    CHECK(lua.text("return tostring(hs.ChildShown('MerchantWnd', 'MW_UsableButton'))") == "false");
    ImGui::DestroyContext(ctx);
}

TEST_CASE("merchant: Usable only hides learned discipline tomes and lower ranks") {
    ImGuiContext* ctx = ImGui::CreateContext();
    mq_test::World w("hailstone_merchant_disciplines_test");
    auto& c = *w.conn;
    auto profile = *c.profile();
    profile.class_ = 7; // Monk: class ID differs from the class bit.
    profile.disciplines.clear();
    c.set_profile_for_test(profile);
    open_merchant(c);

    constexpr std::uint32_t rank_one = 901, rank_two = 902, other_subgroup = 903;
    const auto discipline = [](std::uint32_t id, const char* name, const char* rank, const char* subgroup) {
        return mq_test::spell_line({{0, std::to_string(id)}, {1, name}, {168, "1"},
                                    {207, "910"}, {208, rank}, {236, subgroup}}) + "\n";
    };
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(discipline(rank_one, "Guard", "1", "1") +
                         discipline(rank_two, "Guard Rk. II", "2", "1") +
                         discipline(other_subgroup, "Other Guard", "1", "2")) == 3);
    c.set_spell_table_for_test(table);

    auto tome = [&](std::uint32_t slot, const char* name, std::uint32_t id) {
        auto item = goods(slot, name);
        item.item.classes = 1u << (profile.class_ - 1);
        item.item.item_type = net::kItemTypeScroll;
        item.item.effects[net::kEffectScroll].effect = static_cast<std::int32_t>(id);
        c.receive_zone_packet_for_test(net::encode_item_packet(false, item));
    };
    tome(1, "Tome: Guard", rank_one);
    tome(2, "Tome: Guard II", rank_two);
    tome(3, "Tome: Other Guard", other_subgroup);
    REQUIRE(c.merchant()->items.size() == 3);

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &c;
    app::UiApi api(host);
    Lua lua(api);
    CHECK(lua.text(kNames) == "Tome: Guard,Tome: Guard II,Tome: Other Guard");

    auto learn = [&](std::uint32_t id) {
        ByteWriter data;
        data.le(id);
        for (int n = 1; n < game::mechanics::combat_ability::kCombatAbilities; ++n)
            data.le<std::uint32_t>(0);
        c.receive_zone_packet_for_test(net::make_packet(net::Op::DisciplineUpdate, std::move(data)));
        api.begin_frame();
    };
    learn(rank_one);
    CHECK(lua.text(kNames) == "Tome: Guard II,Tome: Other Guard");
    learn(rank_two);
    CHECK(lua.text(kNames) == "Tome: Other Guard");

    lua.text("hs.ClickChild('MerchantWnd', 'MW_UsableButton', 'left')");
    CHECK(lua.text(kNames) == "Tome: Guard,Tome: Guard II,Tome: Other Guard");
    ImGui::DestroyContext(ctx);
}
