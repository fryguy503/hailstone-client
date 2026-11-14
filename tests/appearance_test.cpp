#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <map>

#include "assets/character.h"
#include "assets/eqg_character.h"
#include "game/actor_look.h"
#include "game/appearance.h"
#include "game/appearance_data.h"
#include "game/race_models.h"
#include "game_files.h"
#include "net/opcodes.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::assets;

TEST_CASE("PC-model races fold TextureType into slots 1-6") {
    game::Appearance a;
    a.texture = 2;
    a.material[0] = 1;
    game::normalize_appearance(a, 1); // human
    CHECK(a.texture == game::kNoTexture);
    CHECK(a.material[0] == 1);
    for (int s = 1; s <= 6; ++s) CHECK(a.material[static_cast<std::size_t>(s)] == 2);

    game::Appearance npc;
    npc.texture = 2;
    game::normalize_appearance(npc, 39); // gnoll: keeps its body texture
    CHECK(npc.texture == 2);
    CHECK(npc.material[1] == 0);
    CHECK(game::is_pc_model_race(522));
    CHECK_FALSE(game::is_pc_model_race(566));
}

TEST_CASE("races without per-slot armour are drawn without armour colours (a skeleton illusion in dyed armour)") {
    game::Appearance a;
    for (std::size_t s = 0; s < 9; ++s) {
        a.material[s] = 3;
        a.tint[s] = 0xffff0000u;
    }
    auto ske = game::drawn_appearance(a, 60);
    for (std::size_t s = 0; s <= game::kSlotFeet; ++s) CHECK(ske.tint[s] == 0u);
    CHECK(ske.tint[game::kSlotPrimary] == 0xffff0000u); // held items keep their own tint index
    CHECK(ske.material == a.material);                 // the armour itself still swaps the textures it has
    for (std::uint32_t race : {1u, 128u, 130u, 330u, 522u}) CHECK(game::drawn_appearance(a, race) == a);
}

TEST_CASE("OP_WearChange follows the client's slot rules") {
    game::Appearance a; // per-slot armour
    CHECK(game::apply_wear_change(a, 1, 3, 0, 0xff102030u));
    CHECK(a.material[1] == 3);
    CHECK(a.tint[1] == 0xff102030u);
    CHECK(game::apply_wear_change(a, 7, 10, 0, 0));
    CHECK(a.material[7] == 10);
    CHECK(game::apply_wear_change(a, 1, 0xffffffffu, 0, 0)); // -1 resets the slot
    CHECK(a.material[1] == 0);
    CHECK_FALSE(game::apply_wear_change(a, 9, 1, 0, 0));

    game::Appearance npc;
    npc.texture = 1; // fixed body texture: body slots ignored, held items not
    CHECK_FALSE(game::apply_wear_change(npc, 2, 3, 0, 0));
    CHECK(npc.material[2] == 0);
    CHECK(game::apply_wear_change(npc, 8, 200, 0, 0));
    CHECK(npc.material[8] == 200);

    // Robes: arms and wrists show the chest's material.
    game::Appearance robe;
    robe.material[1] = 12;
    robe.material[2] = 1;
    CHECK(game::effective_material(robe, 2) == 12);
    CHECK(game::effective_material(robe, 3) == 12);
    CHECK(game::effective_material(robe, 4) == 0);
    CHECK(game::is_robe(10));
    CHECK_FALSE(game::is_robe(17));

    // Luclin models (HandleMaterialEx): arms and wrists take the chest's armour under a robe, a Froglok's legs too.
    CHECK(game::luclin_armor_slot(robe, 5, game::kSlotArms) == game::kSlotChest);
    CHECK(game::luclin_armor_slot(robe, 5, game::kSlotWrists) == game::kSlotChest);
    CHECK(game::luclin_armor_slot(robe, 5, game::kSlotHands) == game::kSlotHands);
    CHECK(game::luclin_armor_slot(robe, 5, game::kSlotLegs) == game::kSlotLegs);
    CHECK(game::luclin_armor_slot(robe, 330, game::kSlotLegs) == game::kSlotChest);
    CHECK(game::luclin_armor_slot(robe, 5, game::kSlotChest) == game::kSlotChest);
    game::Appearance tunic;
    tunic.material[1] = 3;
    CHECK(game::luclin_armor_slot(tunic, 5, game::kSlotArms) == game::kSlotArms);
}

TEST_CASE("a secondary is a shield by its model number, not its textures (CDisplay::IsShield)") {
    const auto is_shield_model = hailstone::game::is_stock_shield_model;
    // The ranges' ends and one past them.
    CHECK_FALSE(is_shield_model(199));
    CHECK(is_shield_model(200));
    CHECK(is_shield_model(228));
    CHECK_FALSE(is_shield_model(229));
    CHECK_FALSE(is_shield_model(300)); // held_type's shield range (IT200..300) is another table
    CHECK(is_shield_model(10530));
    CHECK(is_shield_model(10544));
    CHECK_FALSE(is_shield_model(10545));
    CHECK(is_shield_model(10664));
    CHECK(is_shield_model(10671));
    CHECK_FALSE(is_shield_model(10672));
    CHECK(is_shield_model(11001));
    CHECK(is_shield_model(11020));
    CHECK_FALSE(is_shield_model(11021));
    CHECK(is_shield_model(11496));
    CHECK(is_shield_model(11497));
    CHECK_FALSE(is_shield_model(11498));
    // Single numbers: the 10th anniversary shield IT14000 and the highest, IT67367.
    CHECK(is_shield_model(14000));
    CHECK(is_shield_model(67367));
    CHECK(is_shield_model(11443));
    CHECK_FALSE(is_shield_model(11444));
    // The Haunting Nightmares Shield Ornament's IT101064 (shield_round_halloween textures) is held in the hand.
    CHECK_FALSE(is_shield_model(101064));
    CHECK_FALSE(is_shield_model(0));
}

TEST_CASE("display.extra_shields puts later shields on the shield point (Hailstone)") {
    using hailstone::game::is_shield_model;
    // A shield's model RoF2 does not list, used only by shields and their ornaments.
    CHECK_FALSE(hailstone::game::is_stock_shield_model(11224));
    CHECK(is_shield_model(11224));
    CHECK(is_shield_model(11490));
    // RoF2's own still count.
    CHECK(is_shield_model(200));
    CHECK(is_shield_model(14000));
    // Models only ornaments use, shaped like a shield: a round one, and one whose grip makes it too thick for the
    // measure (tools/gen_shield_models.py SEEN_SHIELDS).
    CHECK(is_shield_model(13908));
    CHECK(is_shield_model(100081));
    CHECK(is_shield_model(100109));
    // The Haunting and Grim Nightmares Shield Ornaments, which RoF2 holds in the hand (not on its list), are worn on
    // the forearm like the other shield ornaments.
    CHECK(is_shield_model(101064));
    CHECK(is_shield_model(101068));
    // Left out: the IT63 placeholder, weapons' models a few shields borrow (IT7, a mace; IT10201, a staff, rod or book;
    // IT10650, a dagger), and weapon ornaments' models (an axe, a sword, a mace).
    CHECK_FALSE(is_shield_model(63));
    CHECK_FALSE(is_shield_model(7));
    CHECK_FALSE(is_shield_model(10201));
    CHECK_FALSE(is_shield_model(10650));
    CHECK_FALSE(is_shield_model(13925));
    CHECK_FALSE(is_shield_model(100069));
    CHECK_FALSE(is_shield_model(100105));
    CHECK_FALSE(is_shield_model(0));
    // Switched off: RoF2's list alone.
    hailstone::game::set_extra_shields(false);
    CHECK_FALSE(is_shield_model(13908));
    CHECK_FALSE(is_shield_model(11224));
    CHECK(is_shield_model(200));
    hailstone::game::set_extra_shields(true);
}

TEST_CASE("OP_WearChange round trip") {
    net::WearChange w;
    w.spawn_id = 0x1234;
    w.armor.material = 22;
    w.armor.hero_forge_model = 7;
    w.tint = 0xff00ff00u;
    w.slot = 8;
    auto p = net::encode(w);
    CHECK(p.data.size() == 27);
    auto d = net::decode_wear_change(p);
    REQUIRE(d);
    CHECK(d->spawn_id == 0x1234);
    CHECK(d->armor.material == 22);
    CHECK(d->armor.hero_forge_model == 7);
    CHECK(d->tint == 0xff00ff00u);
    CHECK(d->slot == 8);
}

TEST_CASE("EQG animation fallback chain follows the race table") {
    CHECK(game::animation_fallback_code("HNM") == "HUM"); // race 566 -> race 1
    CHECK(game::animation_fallback_code("HUM") == "DKM"); // race 1 -> 522
    CHECK(game::animation_fallback_code("RTN").empty());  // no fallback flag
    CHECK(eqg_character::clip_code("STND") == "P01");
    CHECK(eqg_character::clip_code("walk") == "L01");
    CHECK(eqg_character::clip_code("XXXX").empty());
    // The original's mapping (the sequence table and its new-to-old map), not the names' look.
    CHECK(eqg_character::clip_code("PATK") == "C08");
    CHECK(eqg_character::clip_code("BLPR") == "C05");
    CHECK(eqg_character::clip_code("NBOW") == "S28");
    CHECK(eqg_character::clip_code("STUN").empty());
    CHECK(eqg_character::is_track_name("STND_BA_1"));
    CHECK(eqg_character::is_track_name("SLPR_1H_4"));
    CHECK(eqg_character::is_track_name("WALK_MT_2"));
    CHECK_FALSE(eqg_character::is_track_name("STND_BA_5"));
    CHECK_FALSE(eqg_character::is_track_name("STND_XX_1"));
    CHECK_FALSE(eqg_character::is_track_name("DEST_BA_1"));
    CHECK_FALSE(eqg_character::is_track_name("XXXX_BA_1"));
    CHECK_FALSE(eqg_character::is_track_name("STND_BA_1_HUM"));
    auto aliases = eqg_character::classic_aliases(
        [](std::string_view t) { return t == "SLPR_BA_1" || t == "IDLE_BA_1" || t == "SIDL_BA_1"; });
    CHECK(aliases.at("C05") == "SLPR_BA_1");
    CHECK(aliases.at("O01") == "IDLE_BA_1");
    CHECK(aliases.at("O02") == "IDLE_BA_1");
    CHECK(aliases.at("O03") == "SIDL_BA_1");
    CHECK_FALSE(aliases.contains("C06"));
}

TEST_CASE("EQGL material layers: names, texture roles by suffix, entry sizes per version") {
    // Version 4 (8 u32 entries), as gus.lay: the normal map may come first.
    std::string strings;
    auto add = [&](const std::string& s) {
        auto off = static_cast<std::uint32_t>(strings.size());
        strings += s;
        strings.push_back('\0');
        return off;
    };
    std::uint32_t name = add("C_GUS_S01_M01"), normal = add("C_GUS_BD_S00_N.DDS"), diffuse = add("C_GUS_BD_S01_C.DDS"),
                  env = add("C_GUS_EVMP_S05_E.DDS");
    std::vector<std::byte> data;
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) data.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    };
    for (char c : std::string("EQGL")) data.push_back(static_cast<std::byte>(c));
    u32(4);
    u32(static_cast<std::uint32_t>(strings.size()));
    u32(1);
    for (char c : strings) data.push_back(static_cast<std::byte>(c));
    for (std::uint32_t w : {name, normal, diffuse, env, 0xffffffffu, 0xffffffffu, 0x41400000u, 0u}) u32(w);
    auto layers = eqg_character::parse_layers(data);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].name == "C_GUS_S01_M01");
    CHECK(layers[0].diffuse == "C_GUS_BD_S01_C.DDS");
    CHECK(layers[0].normal == "C_GUS_BD_S00_N.DDS");
    CHECK(layers[0].environment == "C_GUS_EVMP_S05_E.DDS");
    REQUIRE(layers[0].shininess);
    REQUIRE(layers[0].env_strength);
    CHECK(*layers[0].shininess == 12.0f);
    CHECK(*layers[0].env_strength == 0.0f);
    auto version3 = data;
    version3[4] = std::byte{3};
    version3.resize(version3.size() - 4);
    auto v3 = eqg_character::parse_layers(version3);
    REQUIRE(v3.size() == 1);
    CHECK(v3[0].shininess == 12.0f);
    CHECK(v3[0].env_strength == 1.0f);
    auto truncated = data;
    truncated.pop_back();
    CHECK_THROWS(eqg_character::parse_layers(truncated));
    auto nonfinite = data;
    nonfinite[nonfinite.size() - 2] = std::byte{0x80};
    nonfinite[nonfinite.size() - 1] = std::byte{0x7f};
    CHECK_THROWS(eqg_character::parse_layers(nonfinite));
    data[4] = std::byte{9}; // unknown version
    CHECK_THROWS(eqg_character::parse_layers(data));
    // Material i (file order) takes layer M<i + 1>.
    CHECK(eqg_character::layer_name("gus", 1, 0) == "C_GUS_S01_M01");
    CHECK(eqg_character::layer_name("GUS", 4, 1) == "C_GUS_S04_M02");
}

TEST_CASE("EQG texture variants come from the model's .lay layers" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto lib = CharacterLibrary::load(game, "");
    auto gus = lib.model("GUS"); // race 549, the small goo
    if (!gus) return;            // an install without gus.eqg
    auto diffuse = [&](int texture) {
        game::Appearance a;
        a.texture = static_cast<std::uint8_t>(texture);
        auto m = lib.dress(*gus, a);
        std::map<std::string, std::string> out; // material name -> lower-case texture
        if (m)
            for (const auto& mat : m->materials)
                if (!mat.frames.empty()) {
                    std::string f = mat.frames.front();
                    for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    out[mat.name] = f;
                }
        return out;
    };
    // Texture 1: the green body (C_GUS_S01_M01), the debris keeps c_gus_gb_s00 (M02).
    auto t1 = diffuse(1);
    CHECK(t1["GUS_m00"] == "c_gus_bd_s01_c.dds");
    CHECK(t1["GUS_m01"] == "c_gus_gb_s00_c.dds");
    // Texture 4: the black set on body and debris, not c_gus_bd_s04_c.dds.
    auto t4 = diffuse(4);
    CHECK(t4["GUS_m00"] == "c_gus_bk_s04_c.dds");
    CHECK(t4["GUS_m01"] == "c_gus_bk_s04_c.dds");
    // Texture 5: mercury; texture 9 has no layer, no c_gus_*_s09 file: unchanged.
    CHECK(diffuse(5)["GUS_m00"] == "c_gus_ev_s05_c.dds");
    CHECK(diffuse(9)["GUS_m00"] == "c_gus_bd_s00_c.dds");
}

TEST_CASE("dressing classic, Luclin and EQG models from the game files" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto lib = CharacterLibrary::load(game, "");
    auto hum = lib.model("HUM");
    REQUIRE(hum);
    CHECK_FALSE(hum->luclin);

    auto frames = [](const CharacterMesh& m) {
        std::vector<std::string> out;
        for (const auto& mat : m.materials)
            for (const auto& f : mat.frames) out.push_back(f);
        return out;
    };
    auto has = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    // Plate (3) on every body slot: HUMCH0301 replaces HUMCH0001 ...
    game::Appearance plate;
    for (int s = 1; s <= 6; ++s) plate.material[static_cast<std::size_t>(s)] = 3;
    plate.tint[1] = 0xffff0000u;
    auto m = lib.dress(*hum, plate);
    REQUIRE(m);
    auto f = frames(*m);
    CHECK(has(f, "HUMCH0301.BMP"));
    CHECK_FALSE(has(f, "HUMCH0001.BMP"));
    // ... tinted, while the bare head stays untinted.
    for (std::size_t i = 0; i < m->materials.size(); ++i) {
        const auto& fr = m->materials[i].frames.front();
        if (fr.starts_with("HUMCH03")) CHECK(m->tints[i] == 0xffff0000u);
        if (fr.starts_with("HUMHE00")) CHECK(m->tints[i] == 0u);
    }
    // The same appearance is cached.
    CHECK(lib.dress(*hum, plate) == m);

    // Face 1 is the tens digit of the head pieces: HUMHE0011.
    game::Appearance face;
    face.face = 1;
    CHECK(has(frames(*lib.dress(*hum, face)), "HUMHE0011.BMP"));

    // A robe swaps the body mesh for HUM01 and uses CLK textures.
    game::Appearance robe;
    robe.material[1] = 10;
    auto r = frames(*lib.dress(*hum, robe));
    CHECK(std::any_of(r.begin(), r.end(), [](const std::string& s) { return s.starts_with("CLK04"); }));

    // Held items hang off the hand bones: IT1 adds geometry bound to R_POINT.
    REQUIRE(lib.has_item(1));
    game::Appearance armed;
    armed.material[7] = 1;
    auto w = lib.dress(*hum, armed);
    REQUIRE(w);
    auto plain = lib.dress(*hum, game::Appearance{});
    CHECK(w->vertices.size() > plain->vertices.size());
    int hand = hum->find_bone("R_POINT_TRACK");
    CHECK(hand >= 0);
    CHECK(w->vertices.back().bones[0] == hand);
    CHECK(hum->find_bone("HAIR_POINT_TRACK") != hand);

    // EQG: Qeynos guards (race 566 HNM) and rodents (415 RTN).
    auto hnm = lib.model("HNM");
    REQUIRE(hnm);
    CHECK(hnm->eqg);
    CHECK(hnm->inverse_bind.size() == hnm->bone_names.size());
    CHECK(hnm->clip("P01"));
    CHECK(hnm->clip("L01"));
    CHECK(hnm->animation_donor == "HUM");
    game::Appearance tex;
    tex.texture = 3;
    auto hf = frames(*lib.dress(*hnm, tex));
    CHECK(has(hf, "c_hnm_bd_s03_c.dds"));
    auto rtn = lib.model("RTN");
    REQUIRE(rtn);
    CHECK(rtn->clip("L01"));
    CHECK(rtn->bounds_min.z > -0.1f); // origin at the feet

    // Luclin humans: layered materials.
    auto luclin = CharacterLibrary::load(game, "", true);
    auto lhum = luclin.model("HUM");
    REQUIRE(lhum);
    CHECK(lhum->luclin);
    CHECK(lhum->clip("C01")); // aliased from C01A
    // Luclin humans: SwapBodyMaterial puts HUMCH0301_MDF on the chest, a
    // skin and an armour layer (SkinMeshLuclin's two textures), with the tint.
    auto ld = luclin.dress(*lhum, plate);
    REQUIRE(ld);
    bool chest = false;
    for (std::size_t i = 0; i < ld->materials.size(); ++i)
        if (ld->materials[i].name == "HUMCH0301_MDF") {
            chest = true;
            CHECK_FALSE(ld->materials[i].tint_layer.empty());
            CHECK(ld->tints[i] == 0xffff0000u);
        }
    CHECK(chest);
    // Luclin hair: IT1000 + style + the race offset, tinted from the 24-colour table.
    game::Appearance hair;
    hair.hair_style = 1;
    hair.hair_color = 10; // 0xfbff81
    auto lh = luclin.dress(*lhum, hair);
    REQUIRE(lh);
    CHECK(std::find(lh->tints.begin(), lh->tints.end(), 0xfffbff81u) != lh->tints.end());
    // Drakkin: the tattoo takes the heritage's BASE_COLOR (Atathus the Red, 0xc80000).
    if (auto dkm = luclin.model("DKM")) {
        game::Appearance d;
        d.drakkin_heritage = 0;
        d.drakkin_tattoo = 3;
        auto dd = luclin.dress(*dkm, d);
        REQUIRE(dd);
        CHECK(std::find(dd->tints.begin(), dd->tints.end(), 0xffc80000u) != dd->tints.end());
    }
}

TEST_CASE("armour tints a classic body slot only where the model has that variant's texture" *
          doctest::skip(!hailstone::test::game_dir())) {
    // SKE's chest variants differ between game folders (one has 00..04, a server's own files may have only 00 and
    // 01), so each of materials 1..9 is checked against what this folder's SKE has: a variant whose SKECH<vv>
    // texture is there takes the colour; one that is not makes ReplaceMaterial fail and the chest stays as it was,
    // untinted. (A skeleton spawn's armour colours are dropped before dressing: game::drawn_appearance.)
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "");
    auto ske = lib.model("SKE");
    REQUIRE(ske);
    REQUIRE_FALSE(ske->luclin);
    int present = 0, missing = 0;
    for (std::uint32_t v = 1; v <= 9; ++v) { // 10..16 are robes, which use other variants
        CAPTURE(v);
        game::Appearance a;
        a.material[game::kSlotChest] = v;
        a.tint[game::kSlotChest] = 0xffff0000u;
        auto m = lib.dress(*ske, a);
        REQUIRE(m);
        const std::string variant = std::format("SKECH{:02}", v);
        bool swapped = false;
        std::vector<std::uint32_t> tints;
        for (std::size_t i = 0; i < m->materials.size(); ++i) {
            const std::string& frame = m->materials[i].frames.front();
            if (!frame.starts_with("SKECH")) continue;
            tints.push_back(m->tints[i]);
            swapped = swapped || frame.starts_with(variant);
        }
        REQUIRE_FALSE(tints.empty());
        ++(swapped ? present : missing);
        for (auto t : tints) CHECK(t == (swapped ? 0xffff0000u : 0u));
    }
    CHECK(missing > 0); // no folder's SKE has chest variants up to 09
    MESSAGE("SKE chest variants 1..9 in this folder: ", present, " present, ", missing, " missing");
}

TEST_CASE("a Luclin robe's sleeves sit on the body's forearms" * doctest::skip(!hailstone::test::game_dir())) {
    // CHierarchicalActor::AttachToHostBones @0x10043e10: the robe's bones take the body bones' matrices of the same
    // names. IT4336's skeleton (the High Elf female's robe 10) holds its sleeve bones (FOROBEARML1..4) away from the
    // body's rest pose; posed by the robe's own rest pose, the sleeves hung detached about 1.4 units below the arms.
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "", true);
    auto hif = lib.model("HIF");
    REQUIRE(hif);
    REQUIRE(hif->luclin);
    game::Appearance robe;
    robe.material[1] = 10;
    auto mesh = lib.dress(*hif, robe);
    REQUIRE(mesh);
    std::vector<glm::mat4> world;
    hif->sample(*hif->clip(""), 0.0f, false, world);
    int forearm = -1, hand = -1;
    std::vector<int> sleeve;
    for (std::size_t i = 0; i < hif->bone_names.size(); ++i) {
        if (hif->bone_names[i] == "HIFFOFOREARML_TRACK") forearm = static_cast<int>(i);
        if (hif->bone_names[i] == "HIFFIHANDL_TRACK") hand = static_cast<int>(i);
        if (hif->bone_names[i].starts_with("HIFFOROBEARML")) sleeve.push_back(static_cast<int>(i));
    }
    REQUIRE(forearm >= 0);
    REQUIRE(hand >= 0);
    REQUIRE(sleeve.size() == 4);
    // Distance from the forearm: the segment from the elbow to the wrist.
    const glm::vec3 elbow(world[static_cast<std::size_t>(forearm)][3]);
    const glm::vec3 wrist(world[static_cast<std::size_t>(hand)][3]);
    std::size_t on_sleeve = 0;
    float farthest = 0.0f;
    for (const auto& v : mesh->vertices) {
        if (std::find(sleeve.begin(), sleeve.end(), static_cast<int>(v.bones[0])) == sleeve.end()) continue;
        ++on_sleeve;
        glm::vec3 p(world[v.bones[0]] * glm::vec4(v.position, 1.0f));
        float t = std::clamp(glm::dot(p - elbow, wrist - elbow) / glm::dot(wrist - elbow, wrist - elbow), 0.0f, 1.0f);
        farthest = std::max(farthest, glm::length(p - (elbow + t * (wrist - elbow))));
    }
    CHECK(on_sleeve > 0);
    CHECK(farthest < 0.6f); // the bell sleeve flares to 0.49; it hung 1.39 away
}

TEST_CASE("SwapHead's head materials 0xf0..0xff (0x40a5b0)") {
    using hailstone::game::head_model;
    using hailstone::game::map_head_material;
    CHECK(map_head_material(1, 0, 0xf0) == 0x273);
    CHECK(map_head_material(1, 1, 0xf0) == 0x26c);
    CHECK(map_head_material(2, 0, 0xf0) == 0x219); // Barbarian: the shared helm
    CHECK(map_head_material(2, 1, 0xf1) == 0x212);
    CHECK(map_head_material(10, 0, 0xf1) == 0x286);
    CHECK(map_head_material(6, 0, 0xf1) == 3);
    CHECK(map_head_material(75, 0, 0xf0) == 0xf0); // not a PC race
    // 0xf0 on a human: item IT627 on the head point, head HE00.
    auto h = head_model(1, 0, 0xf0);
    CHECK(h.item == 0x273);
    CHECK(h.variant == 0);
    // 0xf1: head 3; 7: head 2 (3 for Iksar).
    CHECK(head_model(1, 0, 0xf1).variant == 3);
    CHECK(head_model(1, 0, 7).variant == 2);
    CHECK(head_model(128, 0, 7).variant == 3);
    // A non-PC race's 0xf5 stays >= 0xf0: IT245 with HE00.
    h = head_model(75, 0, 0xf5);
    CHECK(h.item == 0xf5);
    // Vah Shir: helmets are items.
    CHECK(head_model(130, 0, 2).item == 0x29b);
    CHECK(head_model(130, 1, 0xf0).item == 0x297);
    CHECK(head_model(130, 0, 5).item == 0);
}

TEST_CASE("appearance tables: colour tables, NewArmorTagData, NewArmorAdjData") {
    auto t = hailstone::game::ColorTable::parse("[Data]\r\nNumber=3\r\nValue1=255\r\nValue3=0\r\n");
    REQUIRE(t.values.size() == 3);
    CHECK(t.values[0] == 0xff0000ffu);
    CHECK(t.values[1] == 0xffffffffu); // missing: 0xffffff
    CHECK(t.values[2] == 0xff000000u);
    CHECK(hailstone::game::npc_tint_entry(3, 3) == 2);
    CHECK_FALSE(hailstone::game::npc_tint_entry(4, 3));
    CHECK_FALSE(hailstone::game::npc_tint_entry(0, 3));
    // A forged Number fills no more than the client's bound.
    auto forged = hailstone::game::ColorTable::parse("[Data]\nNumber=9223372036854775807\nValue2=1\n");
    CHECK(forged.values.size() == static_cast<std::size_t>(hailstone::game::ColorTable::kMaxEntries));
    CHECK(forged.values[1] == 0xff000001u);
    CHECK(hailstone::game::ColorTable::parse("[Data]\nNumber=-5\n").values.empty());

    hailstone::game::NewArmorTables a;
    a.parse_tags("#ID\tTag\n2000\tL\t14100\t\t\t\t\t# Helm\r\n2000\tLF\t14179\t\t\t\t\t# female\r\n"
                 "2000\tN\t14162\t\t-1\t\t\r\n2005\tL\t14108\t\t\t\t1\r\n");
    CHECK(a.find_tag(2000, "HUM")->it_number == 14100);
    CHECK(a.find_tag(2000, "HUF")->it_number == 14179); // a later line replaces the record
    CHECK(a.find_tag(2000, "DKM")->ignore_adjustment);
    CHECK_FALSE(a.find_tag(2000, "GBN"));
    CHECK(a.find_tag(2005, "ELF")->hide_mesh);
    a.parse_adjustments("BAF,12,2.0,1.0,1.0,0.5,0.0,0.0,90.0,0.0,0.0\n");
    const auto* m = a.find_adjustment(12, 0, "BAF");
    REQUIRE(m);
    // Yaw 90 (about Y): row 0 = (cos, 0, -sin) * 2 -> column 0 = (0, 0, -2); translation (0.5, 0, 0).
    CHECK(m->matrix[0] == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(m->matrix[2] == doctest::Approx(-2.0f));
    CHECK(m->matrix[12] == doctest::Approx(0.5f));
}

namespace {

// A Luclin model's palette for the face and eye functions: material names by palette entry, the
// replace calls they make and which materials are "loaded".
struct FaceEnv : hailstone::game::ActorEnv {
    std::vector<std::string> palette;
    std::vector<std::string> calls;
    std::vector<std::pair<std::string, std::uint32_t>> attached;
    int generic_attachments = 0;
    int armor_attachments = 0;
    bool new_model() override { return false; }
    bool has_dag(std::int32_t) override { return true; }
    bool has_part(std::int32_t) override { return false; }
    std::optional<std::string> part_object(std::int32_t) override { return std::nullopt; }
    bool mesh_hidden(std::int32_t) override { return false; }
    bool attach(std::int32_t dag, const std::string& name, const hailstone::game::NewArmorAdjustment*) override {
        ++generic_attachments;
        attached.emplace_back(name, static_cast<std::uint32_t>(dag));
        return true;
    }
    bool attach_armor(std::int32_t dag, const std::string& name, const hailstone::game::NewArmorAdjustment*) override {
        ++armor_attachments;
        attached.emplace_back(name, static_cast<std::uint32_t>(dag));
        return true;
    }
    void tint_part(std::int32_t, std::uint32_t) override {}
    void overlay_part(std::int32_t, const std::string&) override {}
    bool replace_material(const std::string& from, const std::string& to, std::uint32_t color) override {
        calls.push_back(std::format("{}>{}#{:x}", from, to, color));
        for (auto& p : palette)
            if (p == from) {
                p = to;
                return true;
            }
        return false;
    }
    void show_part(std::int32_t, std::uint8_t, std::int32_t) override {}
    void robe_tint(std::int32_t, std::uint32_t) override {}
    void robe_off(const std::string&) override {}
    void robe_mesh(std::int32_t, const std::string&) override {}
    std::optional<hailstone::game::NewArmorTag> find_new_armor_tag(std::uint32_t, const std::string&) override {
        return std::nullopt;
    }
    const hailstone::game::NewArmorAdjustment* find_adjustment(std::int32_t, std::int32_t, const std::string&) override {
        return nullptr;
    }
    std::uint32_t customization_hair_color(const hailstone::game::ActorLook&, std::uint8_t) override { return 0; }
    void find_eye_materials(std::int32_t& left, std::int32_t& right, bool halfling_male, bool froglok) override {
        for (std::size_t i = 0; i < palette.size(); ++i) {
            std::string_view rest = std::string_view(palette[i]).substr(3);
            if (rest.starts_with("L_EYE")) left = static_cast<std::int32_t>(i);
            else if (rest.starts_with("R_EYE")) right = static_cast<std::int32_t>(i);
            else if (halfling_male && rest.starts_with("R_01")) right = static_cast<std::int32_t>(i);
            else if (froglok && std::string_view(palette[i]).substr(4).starts_with("EYE"))
                left = right = static_cast<std::int32_t>(i);
        }
    }
    bool set_eye_texture(std::uint32_t number, std::int32_t index) override {
        if (index < 0 || static_cast<std::size_t>(index) >= palette.size()) return false;
        palette[static_cast<std::size_t>(index)] = std::format("CHR_EYE{:03}_MDF", number);
        return true;
    }
};

hailstone::game::ActorLook luclin_look(std::int32_t race, std::uint8_t gender, const char* code) {
    hailstone::game::ActorLook look;
    look.race = race;
    look.gender = gender;
    look.code = code;
    look.luclin_model = true;
    return look;
}

} // namespace

namespace {

// A new-style model whose every part exists: UpdateNewArmorSlot's texture and overlay names.
struct ArmorEnv : FaceEnv {
    std::vector<std::string> overlays;
    bool new_model() override { return true; }
    bool has_part(std::int32_t) override { return true; }
    std::optional<std::string> part_object(std::int32_t) override { return std::string("CH"); }
    void overlay_part(std::int32_t, const std::string& name) override { overlays.push_back(name); }
};

} // namespace

TEST_CASE("armour record words 2 and 3 dress as the variation and the overlay material (ActorClient +0x8c)") {
    using namespace hailstone::game;
    hailstone::assets::CharacterModel model;
    model.code = "DRF";
    Appearance a;
    a.material[kSlotChest] = 2;
    a.material_variation[kSlotChest] = 3; // EQEmu Unknown1: ArmorProperties variation (+4)
    a.elite_material[kSlotChest] = 5;     // EQEmu EliteMaterial: ArmorProperties material (+8)
    a.new_armor_type[kSlotChest] = 9;     // newArmorType (+0x10)
    AppearanceData data;
    ActorLook look = hailstone::assets::detail::look_of(model, a, &data);
    CHECK(look.armor[kSlotChest].variation == 3);
    CHECK(look.armor[kSlotChest].material2 == 5);
    CHECK(look.armor[kSlotChest].new_armor_type == 9);
    ArmorEnv env;
    update_new_armor_slot(look, env, kSlotChest);
    REQUIRE_FALSE(env.attached.empty());
    CHECK(env.attached.front().first == "DRF_02_03_CH");
    REQUIRE(env.overlays.size() == 1);
    CHECK(env.overlays.front() == "A_DRF_02_03_S05");
    // Without the elite word there is no overlay, and variation 0 names the base texture.
    a.material_variation[kSlotChest] = 0;
    a.elite_material[kSlotChest] = 0;
    look = hailstone::assets::detail::look_of(model, a, &data);
    ArmorEnv bare;
    update_new_armor_slot(look, bare, kSlotChest);
    REQUIRE_FALSE(bare.attached.empty());
    CHECK(bare.attached.front().first == "DRF_02_00_CH");
    CHECK(bare.overlays.empty());
}

TEST_CASE("hero's forge attachment routing is scoped to resolved armor tags on both model families") {
    using namespace hailstone::game;
    struct TaggedEnv : ArmorEnv {
        bool modern = false;
        bool new_model() override { return modern; }
        std::optional<NewArmorTag> find_new_armor_tag(std::uint32_t id, const std::string&) override {
            if (id != 9) return std::nullopt;
            NewArmorTag tag;
            tag.it_number = 101;
            tag.it_pair = 102;
            return tag;
        }
    };
    for (bool modern : {false, true}) {
        for (bool filtered : {false, true}) {
            TaggedEnv env;
            env.modern = modern;
            ActorLook look = luclin_look(1, 0, "HUM");
            look.new_armor_filtered = filtered;
            look.armor[3].material = 1;
            look.armor[3].new_armor_id = 9;
            apply_armor_texture(look, env, 3);
            REQUIRE(env.attached.size() == 2);
            CHECK(env.armor_attachments == (filtered ? 0 : 2));
            CHECK(env.generic_attachments == (filtered ? 2 : 0));
            if (!filtered) {
                CHECK(env.attached[0].first == "IT101");
                CHECK(env.attached[1].first == "IT102");
            }
        }
        TaggedEnv normal;
        normal.modern = modern;
        ActorLook look = luclin_look(1, 0, "HUM");
        look.armor[3].material = 1;
        apply_armor_texture(look, normal, 3);
        CHECK(normal.armor_attachments == 0);
        CHECK(normal.generic_attachments == 2);
    }
}

TEST_CASE("SwapFace names the Luclin head materials by face variant and index") {
    using namespace hailstone::game;
    FaceEnv env;
    env.palette = {"HUMHE0001_MDF", "HUMHE0004_MDF", "HUMHE0005_MDF", "HUML_EYE_MDF", "HUMR_EYE_MDF"};
    ActorLook look = luclin_look(1, 0, "HUM");
    // Face 13: variant 1, index 3; a human steps pieces 1, 4 and 5.
    CHECK(swap_face(look, env, 13, 0, 1) == 1);
    CHECK(env.palette[0] == "HUMHE0131_MDF");
    CHECK(env.palette[1] == "HUMHE0134_MDF");
    CHECK(env.palette[2] == "HUMHE0135_MDF");
    CHECK(look.face == 13);
    CHECK(look.face_variant == 3);
    // Back to face 2 from 13.
    env.calls.clear();
    CHECK(swap_face(look, env, 2, 13, 1) == 1);
    CHECK(env.palette[0] == "HUMHE0021_MDF");
    CHECK(env.calls.front() == "HUMHE0131_MDF>HUMHE0021_MDF#0");
    // Male elves and female dwarves: the beard colour tints pieces 1 and 3 of a variant face.
    FaceEnv dwarf;
    dwarf.palette = {"DWFHE0001_MDF", "DWFHE0003_MDF", "DWFHE0004_MDF", "DWFHE0005_MDF"};
    ActorLook d = luclin_look(8, 1, "DWF");
    d.beard_color = 4;
    CHECK(swap_face(d, dwarf, 11, 0, 1) == 1);
    CHECK(dwarf.calls[0] == std::format("DWFHE0001_MDF>DWFHE0111_MDF#{:x}", kLuclinHairColors[4]));
    CHECK(dwarf.calls[1] == std::format("DWFHE0003_MDF>DWFHE0113_MDF#{:x}", kLuclinHairColors[4]));
    CHECK(dwarf.calls[2] == "DWFHE0004_MDF>DWFHE0114_MDF#0");
    // A variant above 3 on those races is variant 0.
    FaceEnv elf;
    elf.palette = {"ELMHE0001_MDF"};
    ActorLook e = luclin_look(5, 0, "ELM");
    swap_face(e, elf, 42, 0, 1);
    CHECK(elf.palette[0] == "ELMHE0021_MDF");
}

TEST_CASE("Luclin eyes: CHR_EYE<colour + race offset> on the eye palette entries, eye patches") {
    using namespace hailstone::game;
    FaceEnv env;
    env.palette = {"HUMHE0001_MDF", "HUML_EYE_MDF", "HUMR_EYE_MDF"};
    ActorLook look = luclin_look(1, 0, "HUM");
    find_eye_materials(look, env);
    CHECK(look.eye_left == 1);
    CHECK(look.eye_right == 2);
    look.eye_color = 5;
    CHECK(set_fheb(look, env, 5, 3, 0) == 1);
    CHECK(env.palette[1] == "CHR_EYE005_MDF");
    CHECK(env.palette[2] == "CHR_EYE005_MDF");
    // Face 3 of a male human (the eye patch) puts material 0xd1 on the right eye.
    look.face = 3;
    CHECK(set_luclin_eye(look, env, true, 5) == 1);
    CHECK(env.palette[2] == "CHR_EYE209_MDF");
    CHECK(look.eye_color2 == 0xd1);
    // Race offsets: an Ogre's colour 2 is CHR_EYE082.
    FaceEnv ogre;
    ogre.palette = {"OGML_EYE_MDF", "OGMR_EYE_MDF"};
    ActorLook o = luclin_look(10, 0, "OGM");
    find_eye_materials(o, ogre);
    CHECK(set_luclin_eye(o, ogre, false, 2) == 1);
    CHECK(ogre.palette[0] == "CHR_EYE082_MDF");
    // No right eye entry or colour 0xff: nothing.
    ActorLook none = luclin_look(1, 0, "HUM");
    CHECK(set_luclin_eye(none, ogre, false, 2) == 0);
    CHECK(set_luclin_eye(o, ogre, false, 0xff) == 0);
    // A male Halfling's right eye is its "R_01" entry; a Froglok's "?EYE" entry is both.
    FaceEnv halfling;
    halfling.palette = {"HAML_EYE_MDF", "HAMR_01_MDF"};
    ActorLook h = luclin_look(11, 0, "HAM");
    find_eye_materials(h, halfling);
    CHECK(h.eye_right == 1);
    FaceEnv frog;
    frog.palette = {"FRMHE0001_MDF", "FRMEEYE_MDF"};
    ActorLook f = luclin_look(330, 0, "FRM");
    find_eye_materials(f, frog);
    CHECK(f.eye_left == 1);
    CHECK(f.eye_right == 1);
}
