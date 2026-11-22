// The RoF2 sound triggers that live in the packet handlers and the bag windows (docs/formats/audio.md "Which sound
// for which event"): an item moved by a click, the adventure finish, a quest reward, a GM kill, a bag opening
// or closing, and the water under the feet. Each is a wave cue.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "app/item_ui.h"
#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "app/player_movement.h"
#include "app/sound_events.h"
#include "audio/individual_sounds.h"
#include "assets/regions.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "mq_test_world.h"
#include "net/game_connection.h"
#include "net/item_packets.h"
#include "net/packet_io.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

std::vector<std::int32_t> wave_cues(net::GameConnection& c) {
    std::vector<std::int32_t> out;
    for (const net::SoundCue& cue : c.take_sound_cues())
        if (cue.kind == net::SoundCue::Kind::Wave) out.push_back(static_cast<std::int32_t>(cue.spell));
    return out;
}

// The context each Wave cue carries (Hailstone: what the Individual Sounds volumes are picked from).
std::vector<net::SoundCue::Context> context_cues(net::GameConnection& c) {
    std::vector<net::SoundCue::Context> out;
    for (const net::SoundCue& cue : c.take_sound_cues())
        if (cue.kind == net::SoundCue::Kind::Wave) out.push_back(cue.context);
    return out;
}

constexpr std::int16_t kHatPackSlot = net::kSlotGeneral1 + 2, kOtherPackSlot = net::kSlotGeneral1 + 3;
constexpr std::uint32_t kHatItemId = 6001;
constexpr std::uint32_t kBagType = 7; // a bag type with the bag's own open wave
constexpr std::uint32_t kPetOwnerId = 99, kNoOwner = 0;

net::Item make_hat() {
    net::Item hat;
    hat.name = "Hat";
    hat.id = kHatItemId;
    hat.equip_slots = 1u << net::kSlotHead;
    hat.classes = 0xffff;
    return hat;
}

} // namespace

TEST_CASE("Individual sounds: an item put on or taken off carries the ItemWorn context, a move between packs none") {
    mq_test::World w("sound_equip_group");
    auto& c = *w.conn;
    using A = net::GameConnectionTestAccess;
    using X = net::SoundCue::Context;
    auto poss = [](std::int16_t s) { return net::ItemSlot::possessions(s); };
    REQUIRE(A::inventory(c).put(poss(kHatPackSlot), make_hat()));
    c.take_sound_cues();
    c.click_slot(poss(kHatPackSlot));        // picked up from a pack
    CHECK(context_cues(c) == std::vector<X>{X::None});
    c.click_slot(poss(net::kSlotHead));      // put on
    CHECK(context_cues(c) == std::vector<X>{X::ItemWorn});
    c.click_slot(poss(net::kSlotHead));      // taken off
    CHECK(context_cues(c) == std::vector<X>{X::ItemWorn});
    c.click_slot(poss(kOtherPackSlot));      // put into a pack: nothing carries over from the move before
    CHECK(context_cues(c) == std::vector<X>{X::None});
}

TEST_CASE("Individual sounds: /autoinventory into a worn slot gives both of its cues the ItemWorn context") {
    mq_test::World w("sound_equip_autoinventory");
    auto& c = *w.conn;
    using A = net::GameConnectionTestAccess;
    using X = net::SoundCue::Context;
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(net::kSlotCursor), make_hat()));
    c.take_sound_cues();
    // AutoInventoryCursorItem's own wave, then cmd_autoinventory's: both are an item put on, so both are silenced
    // together by the Equip volume (a second cue with no context used to play at full volume).
    A::item_command(c, "/autoinventory", "");
    REQUIRE(A::inventory(c).at(net::ItemSlot::possessions(net::kSlotHead)));
    CHECK(context_cues(c) == std::vector<X>{X::ItemWorn, X::ItemWorn});

    // The paper doll's drop (CInventoryWnd::HandleLButtonUp) is the same: it equips, then plays its own wave.
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(net::kSlotCursor), make_hat()));
    static_cast<void>(A::inventory(c).take(net::ItemSlot::possessions(net::kSlotHead)));
    c.take_sound_cues();
    A::drop_on_doll(c);
    REQUIRE(A::inventory(c).at(net::ItemSlot::possessions(net::kSlotHead)));
    CHECK(context_cues(c) == std::vector<X>{X::ItemWorn, X::ItemWorn});

    // Into the packs: neither cue is an equip sound.
    net::Item rock;
    rock.name = "Rock";
    rock.id = kHatItemId + 1;
    rock.item_class = 0;
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(net::kSlotCursor), rock));
    c.take_sound_cues();
    A::item_command(c, "/autoinventory", "");
    CHECK(context_cues(c) == std::vector<X>{X::None});
}

TEST_CASE("Individual sounds: /autoinventory merging part of a stack into a worn slot still counts as equipping") {
    constexpr std::uint32_t kArrowId = 8005, kArrowType = 27, kMaxStack = 100, kWornCount = 90, kHeldCount = 15;
    mq_test::World w("sound_equip_partial_merge");
    auto& c = *w.conn;
    using A = net::GameConnectionTestAccess;
    using X = net::SoundCue::Context;
    net::Item worn;
    worn.id = kArrowId;
    worn.name = "Field Point Arrow";
    worn.item_type = kArrowType;
    worn.max_stack = kMaxStack;
    worn.stack_count = kWornCount;
    worn.classes = 0xffff;
    worn.equip_slots = 1u << net::kSlotAmmo;
    net::Item held = worn;
    held.stack_count = kHeldCount;
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(net::kSlotAmmo), worn));
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(net::kSlotCursor), held));
    c.take_sound_cues();
    // The 10 that fit go onto the worn stack, the other 5 into a pack: the wave that follows is an equip sound.
    A::item_command(c, "/autoinventory", "");
    REQUIRE_FALSE(A::inventory(c).cursor());
    CHECK(context_cues(c) == std::vector<X>{X::ItemWorn});
}

TEST_CASE("Individual sounds: a bag opens in the BagOpen context, in a bank slot BankBagOpen") {
    mq_test::World w("sound_bag_group");
    auto& c = *w.conn;
    using X = net::SoundCue::Context;
    c.take_sound_cues();
    c.container_sound(kBagType, true);
    c.container_sound(kBagType, true, true);
    c.container_sound(kBagType, false); // closing has no volume of its own
    c.container_sound(kBagType, false, true);
    CHECK(context_cues(c) == std::vector<X>{X::BagOpen, X::BankBagOpen, X::None, X::None});
}

TEST_CASE("Individual sounds: the group of a Wave cue and of a spawn's footsteps") {
    using G = audio::SoundGroup;
    net::SoundCue cue;
    cue.kind = net::SoundCue::Kind::Wave;
    cue.spell = static_cast<std::uint32_t>(game::kSoundCoin);
    CHECK(app::wave_cue_group(cue) == G::Money);
    cue.spell = static_cast<std::uint32_t>(game::kSoundItemMoved);
    CHECK(app::wave_cue_group(cue) == G::None);
    cue.context = net::SoundCue::Context::ItemWorn;
    CHECK(app::wave_cue_group(cue) == G::Equip);
    cue.context = net::SoundCue::Context::BagOpen;
    CHECK(app::wave_cue_group(cue) == G::OpenBag);
    cue.context = net::SoundCue::Context::BankBagOpen;
    CHECK(app::wave_cue_group(cue) == G::OpenBank);

    game::Spawn player;                 // another player: not an NPC, whatever its owner field says
    player.is_npc = false;
    player.pet_owner_id = kPetOwnerId;
    game::Spawn pet;                    // an NPC with an owner
    pet.is_npc = true;
    pet.pet_owner_id = kPetOwnerId;
    game::Spawn npc;                    // an NPC without one
    npc.is_npc = true;
    npc.pet_owner_id = kNoOwner;
    CHECK(app::footstep_group(player, true) == G::OwnFootsteps);
    CHECK(app::footstep_group(player, false) == G::OtherFootsteps);
    CHECK(app::footstep_group(pet, false) == G::PetFootsteps);
    CHECK(app::footstep_group(npc, false) == G::OtherFootsteps);

    // Every group the options list has a wave to test with; the others have none.
    for (const audio::IndividualSound& sound : audio::individual_sounds()) CHECK(app::test_wave(sound.group) > 0);
    CHECK(app::test_wave(G::None) == 0);
}

TEST_CASE("Item sound: every click that moves an item plays 0x92, MoveItem itself plays nothing") {
    mq_test::World w("sound_item_moved");
    auto& c = *w.conn;
    using A = net::GameConnectionTestAccess;
    auto poss = [](std::int16_t s) { return net::ItemSlot::possessions(s); };
    net::Item hat;
    hat.name = "Hat";
    hat.id = 6001;
    hat.equip_slots = 1u << net::kSlotHead;
    hat.classes = 0xffff;
    REQUIRE(A::inventory(c).put(poss(net::kSlotGeneral1 + 2), hat));
    c.take_sound_cues();

    // CInvSlot::HandleLButtonUp: the pick-up and the put-down each play once (@0x697148), into a worn slot or out of
    // one alike.
    c.click_slot(poss(net::kSlotGeneral1 + 2));
    REQUIRE(A::inventory(c).at(poss(net::kSlotCursor)));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x92});
    c.click_slot(poss(net::kSlotHead));
    REQUIRE(A::inventory(c).at(poss(net::kSlotHead)));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x92});
    c.click_slot(poss(net::kSlotHead));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x92});
    // A click the client refuses moves nothing and is silent: the hat does not go on the chest.
    c.click_slot(poss(net::kSlotChest));
    CHECK(wave_cues(c).empty());
    REQUIRE(A::inventory(c).at(poss(net::kSlotCursor)));
    c.click_slot(poss(net::kSlotGeneral1 + 3));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x92});

    // A stack taken through the quantity dialog (Shift: all of it at once) is CInvSlot::SliderComplete's move,
    // which plays nothing; putting it down again is an ordinary click.
    c.click_slot(poss(net::kSlotGeneral1), true, false);
    REQUIRE(A::inventory(c).at(poss(net::kSlotCursor)));
    CHECK(wave_cues(c).empty());
    c.click_slot(poss(net::kSlotGeneral1));
    REQUIRE(A::inventory(c).at(poss(net::kSlotGeneral1)));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x92});

    // CInvSlotMgr::MoveItem has no sound of its own.
    A::move_item(c, poss(net::kSlotGeneral1 + 3), poss(net::kSlotHead), 1);
    REQUIRE(A::inventory(c).at(poss(net::kSlotHead)));
    CHECK(wave_cues(c).empty());
}

TEST_CASE("Item sound: /autoinventory into an empty worn slot plays twice, into the packs once") {
    mq_test::World w("sound_autoinventory");
    auto& c = *w.conn;
    using A = net::GameConnectionTestAccess;
    net::Item hat;
    hat.name = "Hat";
    hat.id = 6001;
    hat.equip_slots = 1u << net::kSlotHead;
    hat.classes = 0xffff;
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(net::kSlotCursor), hat));
    c.take_sound_cues();
    // AutoInventoryCursorItem's own wave for the worn slot (@0x4fe388), then cmd_autoinventory's for the empty
    // cursor (@0x4fe987).
    A::item_command(c, "/autoinventory", "");
    REQUIRE(A::inventory(c).at(net::ItemSlot::possessions(net::kSlotHead)));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x92, 0x92});

    net::Item rock;
    rock.name = "Rock";
    rock.id = 6002;
    rock.item_class = 0;
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(net::kSlotCursor), rock));
    A::item_command(c, "/autoinventory", "");
    REQUIRE_FALSE(A::inventory(c).at(net::ItemSlot::possessions(net::kSlotCursor)));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x92});
}

TEST_CASE("Adventure finish: won plays 0x8d, failed 0x8c") {
    mq_test::World w("sound_adventure");
    auto& c = *w.conn;
    for (bool won : {true, false}) {
        ByteWriter finish;
        finish.le<std::uint32_t>(won ? 1u : 0u);
        finish.le<std::uint32_t>(won ? 50u : 0u);
        c.receive_zone_packet_for_test(net::make_packet(net::Op::AdventureFinish, std::move(finish)));
        CHECK(wave_cues(c) == std::vector<std::int32_t>{won ? 0x8d : 0x8c});
    }
}

TEST_CASE("Quest reward: OP_Sound plays 0x8d, OP_CashReward only credits the coin") {
    mq_test::World w("sound_coin");
    auto& c = *w.conn;
    const net::Coins before = net::GameConnectionTestAccess::coins(c);
    // OP_CashReward goes to a handler that adds the coin and never plays (0x4c636d -> 0x4bcef0).
    ByteWriter cash;
    for (std::uint32_t v : {1u, 2u, 3u, 4u}) cash.le<std::uint32_t>(v);
    c.receive_zone_packet_for_test(net::make_packet(net::Op::CashReward, std::move(cash)));
    CHECK(wave_cues(c).empty());
    const net::Coins& credited = net::GameConnectionTestAccess::coins(c);
    CHECK(credited.platinum == before.platinum + 4); // copper, silver, gold, platinum on the wire
    CHECK(credited.copper == before.copper + 1);

    // OP_Sound's QuestReward_Struct: another player's id stays silent, ours plays even with no coin (0x4c7ff6).
    auto quest = [](std::uint32_t target, bool coins) {
        ByteWriter q;
        q.zeros(4);
        q.le<std::uint32_t>(target);
        q.zeros(12);
        for (std::uint32_t v : {coins ? 1u : 0u, 0u, 0u, 0u}) q.le<std::uint32_t>(v);
        return net::make_packet(net::Op::Sound, std::move(q));
    };
    c.receive_zone_packet_for_test(quest(7, true));
    CHECK(wave_cues(c).empty());
    c.receive_zone_packet_for_test(quest(1, false));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x8d});
}

TEST_CASE("GM kill: the struck-down wave (0x8f) only for our own name") {
    mq_test::World w("sound_gmkill");
    auto& c = *w.conn;
    while (c.poll_chat()) {}
    ByteWriter other;
    other.fixed_string("Someoneelse", 12);
    c.receive_zone_packet_for_test(net::make_packet(net::Op::GMKill, std::move(other)));
    CHECK(wave_cues(c).empty());
    CHECK_FALSE(c.poll_chat());

    ByteWriter mine;
    mine.fixed_string("tester", 6); // the local player's name, without case
    c.receive_zone_packet_for_test(net::make_packet(net::Op::GMKill, std::move(mine)));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x8f});
    auto line = c.poll_chat();
    REQUIRE(line);
    CHECK(line->color == 10);
}

TEST_CASE("GM kill: the named player is left at -10000 HP, alive, until the server's next HP update") {
    // The case at 0x4c8a23 kills locally after the wave: profile HP and the spawn's HPCurrent to -10000, the
    // RespawnTimer cleared, no OP_Death and no respawn window. Stock EQEmu does not kill the player for it.
    mq_test::World w("gmkill_hp");
    auto& c = *w.conn;
    auto& status = net::GameConnectionTestAccess::status(c);
    status.hp = 500;
    status.max_hp = 1000;
    auto& world = net::GameConnectionTestAccess::world(c);
    REQUIRE(world.spawns.count(world.player_id));
    world.spawns.at(world.player_id).hp_percent = 50;

    ByteWriter other;
    other.fixed_string("Someoneelse", 64);
    other.fixed_string("Gm", 64);
    other.le<std::uint32_t>(0);
    c.receive_zone_packet_for_test(net::make_packet(net::Op::GMKill, std::move(other)));
    CHECK(status.hp == 500);
    CHECK(world.spawns.at(world.player_id).hp_percent == 50);

    ByteWriter mine;
    mine.fixed_string("TESTER", 64);
    mine.fixed_string("Gm", 64);
    mine.le<std::uint32_t>(0);
    c.receive_zone_packet_for_test(net::make_packet(net::Op::GMKill, std::move(mine)));
    CHECK(status.hp == -10000);
    CHECK(c.own_hp().cur < 1);
    CHECK(world.spawns.at(world.player_id).hp_percent == 0);
    CHECK_FALSE(status.dead);
    CHECK(status.owner_status == 0);
    CHECK_FALSE(c.respawn_window().has_value());

    ByteWriter hp;
    hp.le<std::int16_t>(static_cast<std::int16_t>(world.player_id));
    hp.le<std::uint32_t>(1000);
    hp.le<std::int32_t>(1000);
    c.receive_zone_packet_for_test(net::make_packet(net::Op::HPUpdate, std::move(hp)));
    CHECK(status.hp == 1000);
    CHECK(world.spawns.at(world.player_id).hp_percent == 100);
}

TEST_CASE("Bags: a bag of a type with its own sound opens and closes (0x86/0x85), once each") {
    mq_test::World w("sound_container");
    auto& c = *w.conn;
    // The fixture's bag, type 6: the first table entry is its own, not the shared container sound.
    net::Item* bag = net::GameConnectionTestAccess::inventory(c).at(
        net::ItemSlot::possessions(net::kSlotGeneral1 + 1));
    REQUIRE(bag);
    bag->bag_type = 6;

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &c;
    app::reset_containers();
    c.take_sound_cues();

    app::open_all_bags(host);
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x86});
    // An open bag does not open again, so the second press is silent.
    app::open_all_bags(host);
    CHECK(wave_cues(c).empty());
    app::close_all_bags(host);
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x85});
    app::close_all_bags(host);
    CHECK(wave_cues(c).empty());

    // An open bag picked up closes with its own close wave (CloseContainer always ends in it), after the item
    // sound; the windows notice it on the next frame.
    app::open_all_bags(host);
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x86});
    c.click_slot(net::ItemSlot::possessions(net::kSlotGeneral1 + 1));
    REQUIRE(net::GameConnectionTestAccess::inventory(c).at(net::ItemSlot::possessions(net::kSlotCursor)));
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x92});
    app::prune_bag_windows(c);
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x85});
    app::prune_bag_windows(c);
    CHECK(wave_cues(c).empty());
    app::reset_containers();
}

TEST_CASE("Item windows: a bag closes with the type it had when it opened") {
    mq_test::World w("sound_item_ui_bags");
    auto& c = *w.conn;
    using A = net::GameConnectionTestAccess;
    const net::ItemSlot bag_slot = net::ItemSlot::possessions(net::kSlotGeneral1 + 1);
    net::Item* bag = A::inventory(c).at(bag_slot);
    REQUIRE(bag);
    bag->bag_type = 7; // one of the own-wave types at 0x65d180 (6, 7, 10, 15, 18, 19, 20, 22): 0x85

    app::ItemUi ui; // no icons: the bag windows and their sounds do not need them
    c.take_sound_cues();
    ui.open_bag(c, net::kSlotGeneral1 + 1);

    // The bag leaves and another item with a type of its own takes the slot: the close uses the bag that stood
    // there, never the replacement (type 2 would take the shared wave 0x91).
    auto gone = A::inventory(c).take(bag_slot);
    REQUIRE(gone);
    net::Item rock;
    rock.name = "Rock";
    rock.id = 6002;
    rock.bag_type = 2;
    REQUIRE(A::inventory(c).put(bag_slot, rock));

    ui.close_all_bags(c);
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x85});
    // Nothing stays open: the second close is silent.
    ui.close_all_bags(c);
    CHECK(wave_cues(c).empty());
}

TEST_CASE("Item windows: hiding the bank closes a carried bag with its close wave") {
    mq_test::World w("sound_item_ui_bank");
    auto& c = *w.conn;
    using A = net::GameConnectionTestAccess;
    net::Item* bag = A::inventory(c).at(net::ItemSlot::possessions(net::kSlotGeneral1 + 1));
    REQUIRE(bag);
    bag->bag_type = 6; // Chest_Cl.WAV 0x85

    app::ItemUi ui;
    A::bank(c) = net::BankSession{};
    A::bank(c)->serial = 7;
    // The frame with the bank window open: it records the session, so the next change is a hide.
    ui.close_bags_when_bank_hides(c);
    ui.open_bag(c, net::kSlotGeneral1 + 1);
    c.take_sound_cues();

    A::bank(c).reset(); // the window hides
    ui.close_bags_when_bank_hides(c);
    CHECK(wave_cues(c) == std::vector<std::int32_t>{0x85});
    // The hide already closed it: the next frame is silent.
    ui.close_bags_when_bank_hides(c);
    CHECK(wave_cues(c).empty());
}

TEST_CASE("Water: entering and leaving a region under the feet plays 0x63 only on a change") {
    // A flat plane (world = nullptr) and one lava box ("LAN_", an LA tag of at least four characters, type 7):
    // walking in and out plays the water-out wave, and staying inside is silent.
    test::ScratchDir scratch("sound_water");
    GameDir game(scratch.path());
    net::GameConnection conn(game, {});
    assets::RegionBox box;
    box.name = "LAN_";
    box.center = {60.0f, 0.0f, 0.0f};
    box.extents = {30.0f, 200.0f, 50.0f}; // x 30..90, z -50..50: the feet's region sample stays inside
    const assets::ZoneRegions regions = assets::ZoneRegions::from_boxes({box});
    REQUIRE_FALSE(regions.empty());
    REQUIRE((regions.lookup({0.0f, 0.0f, 0.25f}).flags & 0xff) == 0);
    REQUIRE((regions.lookup({60.0f, 0.0f, 0.25f}).flags & 0xff) == 7);

    constexpr float kAvatarHeight = 3.75f;
    app::PlayerMovement m;
    m.reset({0.0f, 0.0f, 0.0f}, 0.0f);
    app::MovementKeys keys;
    keys.forward = true;
    constexpr std::int32_t kWaterOut = 0x63;
    std::uint32_t now = 10000;
    std::vector<std::int32_t> cues;
    int inside_frames = 0;
    int enters = 0;
    int leaves = 0;
    bool left = false;
    int frames_after_leave = 0;
    // The wave belongs to the region sample PlayerMovement takes before the physics step: the feet as the last
    // frame left them, one frame behind frame.feet. Keep the two latest positions and compare each frame with
    // the sample its cue comes from.
    glm::vec3 prev1 = {0.0f, 0.0f, 0.0f}; // this frame's pre-step sample
    glm::vec3 prev2 = prev1;              // the sample before it (frame 0's probe is the reset position)
    auto in_region = [&](const glm::vec3& feet) {
        // CheckRegionChange's sample point (region_sample_z): the feet + 0.25 for a standing body with no length.
        return (regions.lookup({feet.x, feet.y, feet.z + 0.25f}).flags & 0xff) != 0;
    };
    for (int frame_no = 0; frame_no < 3000; ++frame_no) {
        now += 16;
        const auto frame = m.update(keys, 0.0f, 0.0f, 16.0f, now, conn, &regions, kAvatarHeight, nullptr);
        if (in_region(frame.feet)) ++inside_frames;
        // A crossing at the pre-step sample sounds on this frame; a frame between crossings is silent.
        const bool before = in_region(prev1);
        const bool earlier = in_region(prev2);
        if (before != earlier) {
            CHECK(frame.water_sounds == std::vector<std::int32_t>{kWaterOut});
            if (before) {
                ++enters;
            } else {
                ++leaves;
                left = true;
            }
        } else {
            CHECK(frame.water_sounds.empty());
        }
        for (std::int32_t s : frame.water_sounds) cues.push_back(s);
        prev2 = prev1;
        prev1 = frame.feet;
        // The leave cue lands on the frame after the feet left the box; check a few dry frames past it too.
        if (left && ++frames_after_leave > 3) break;
    }
    REQUIRE(inside_frames > 1);
    CHECK(enters == 1);
    CHECK(leaves == 1);
    CHECK(cues == std::vector<std::int32_t>{kWaterOut, kWaterOut});
}
