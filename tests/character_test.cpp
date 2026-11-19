#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <tuple>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/character.h"
#include "assets/eqg.h"
#include "assets/eqg_character.h"
#include "assets/pfs.h"
#include "assets/spell_effects.h"
#include "core/game_dir.h"
#include "game/appearance.h"
#include "game/appearance_data.h"
#include "game/race_models.h"
#include "game_files.h"
#include "scratch_dir.h"
#include "pfs_fixtures.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;

TEST_CASE("RoF2 guild banner uses its cloth layers, second UVs and independent tints") {
    const auto game_dir = test::game_dir();
    if (!game_dir) return;
    auto library = CharacterLibrary::load(*game_dir, "");
    const auto model = library.model("G00");
    REQUIRE(model);
    const game::Appearance plain;
    const auto base = library.dress(*model, plain);
    REQUIRE(base);
    game::Appearance banner = plain;
    banner.banner = core::BannerAppearance{{2, 5}, {0xff123456u, 0xff000000u}};
    const auto dressed = library.dress(*model, banner);
    REQUIRE(dressed);
    REQUIRE(dressed->materials.size() == 2);
    CHECK(dressed->materials[0].frames == std::vector<std::string>{"C_GDB_FLAG1_S02_M00_C.DDS"});
    CHECK(dressed->materials[0].tint_layer == "C_GDB_FLAG2_S05_M00_C.DDS");
    CHECK(dressed->materials[0].banner_base_tint == 0xff123456u);
    CHECK(dressed->tints[0] == 0xff010101u);
    CHECK(dressed->materials[1].frames == base->materials[1].frames);
    CHECK(dressed->materials[1].banner_base_tint == 0);
    CHECK(base->materials[0].banner_base_tint == 0);
    CHECK(std::any_of(dressed->vertices.begin(), dressed->vertices.end(), [](const SkinnedVertex& v) {
        return v.uv != v.uv2;
    }));
    banner.banner->pattern[1] = 6;
    const auto changed = library.dress(*model, banner);
    REQUIRE(changed);
    CHECK(changed != dressed);
    CHECK(changed->materials[0].tint_layer == "C_GDB_FLAG2_S06_M00_C.DDS");
    CHECK(dressed->materials[0].tint_layer == "C_GDB_FLAG2_S05_M00_C.DDS");
}

TEST_CASE("WLD blood attachment uses the authored DAG name, independently of its track") {
    using namespace test::wld_fixtures;
    struct NameRole { const char* code; const char* dag; int bone; };
    for (const auto role : std::array{
            NameRole{"TST", "TSTHEAD_POINT_DAG", 2}, NameRole{"TST", "TSTHE_DAG", 2},
            NameRole{"TST", "HUMHEAD_POINT_DAG", 2}, NameRole{"TST", "HUFHEAD_POINT_DAG", -1},
            NameRole{"HUF", "HUFHEAD_POINT_DAG", 2}, NameRole{"HUF", "HUMHEAD_POINT_DAG", -1}}) {
    for (bool blood_dag : {false, true}) {
        WldBuilder b;
        auto bitmap = b.add(0x03, b.str("A_SPRITE"), bitmap_name("fixture.bmp"));
        ByteWriter info;
        info.le<std::uint32_t>(0);
        info.le<std::uint32_t>(1);
        info.le(bitmap);
        auto sprite_info = b.add(0x04, 0, info);
        ByteWriter ref;
        ref.le(sprite_info);
        ref.le<std::uint32_t>(0);
        auto sprite = b.add(0x05, 0, ref);
        auto mat = b.add(0x30, b.str("A_MDF"), material(0x80000001, sprite));
        auto materials = b.add(0x31, 0, refs({mat, mat, mat}));
        auto geometry = b.add(0x36, b.str("TST_DMSPRITEDEF"), mesh(materials, false));
        ref = {};
        ref.le(geometry);
        ref.le<std::uint32_t>(0);
        auto mesh_ref = b.add(0x2d, 0, ref);
        ByteWriter track;
        track.le<std::int32_t>(0);
        track.le<std::uint32_t>(0);
        const std::array tracks{b.add(0x13, b.str("TST_BLD_TRACK"), track),
                                b.add(0x13, b.str("TST_ARM_TRACK"), track),
                                b.add(0x13, b.str("TST_ROOT_TRACK"), track)};
        ByteWriter skeleton;
        skeleton.le<std::uint32_t>(0); // the root node owns this rigid fixture mesh
        skeleton.le<std::uint32_t>(3);
        skeleton.le<std::int32_t>(0);
        const std::array<std::string, 3> dags{"TST_ROOT_DAG", blood_dag ? std::string(role.code) + "_BLD_DAG" : "OTHER_BLD_DAG", role.dag};
        for (std::size_t i = 0; i < dags.size(); ++i) {
            skeleton.le(b.str(dags[i]));
            skeleton.le<std::uint32_t>(0);
            skeleton.le(tracks[i]);
            skeleton.le<std::int32_t>(i == 0 ? mesh_ref : 0);
            skeleton.le<std::uint32_t>(0);
        }
        auto skeleton_def = b.add(0x10, b.str("TST_HS_DEF"), skeleton);
        ref = {};
        ref.le(skeleton_def);
        ref.le<std::uint32_t>(0);
        auto skeleton_ref = b.add(0x11, 0, ref);
        ByteWriter actor;
        for (std::uint32_t v : {0u, 0u, 0u, 1u, 0u}) actor.le(v);
        actor.le(skeleton_ref);
        b.add(0x14, b.str(std::string(role.code) + "_ACTORDEF"), actor);
        auto archive = std::make_shared<CharacterArchive>();
        archive->archive = std::make_shared<PfsArchive>(PfsArchive::from_memory(
            test::pfs_fixtures::make_pfs({{"fixture.wld", b.build()}}), "blood_fixture.s3d"));
        archive->wlds.push_back(wld::WldFile::parse(*archive->archive->read("fixture.wld"), "fixture.wld"));
        CharacterLibrary library({archive});
        auto model = library.build(role.code);
        REQUIRE(model.bone_names.size() == 3);
        CHECK(model.bone_names[0] == "TST_BLD_TRACK"); // a misleading track is not a blood point
        CHECK(model.bone_names[1] == "TST_ARM_TRACK");
        CHECK(model.blood_bone == (blood_dag ? 1 : -1));
        CHECK(model.name_bone == role.bone);
    }
    }
    CHECK(CharacterModel{}.blood_bone == -1);
}

TEST_CASE("UseLuclinElementals gates the implicit global NPC archive, not listed ones") {
    test::ScratchDir dir("elemental_archives");
    std::filesystem::create_directories(dir.path() / "Resources");
    for (const char* name : {"global5_chr.s3d", "global5_chr2.s3d", "frog_mount_chr.s3d", "other_chr.s3d"})
        std::ofstream(dir.path() / name) << "fixture";
    auto has = [](const std::vector<std::string>& list, const char* name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    auto write_ini = [&](const char* value) {
        std::ofstream(dir.path() / "eqclient.ini") << "[Defaults]\nUseLuclinElementals=" << value << '\n';
    };
    // No GlobalLoad entry names global5_chr: only the implicit load brings it in.
    std::ofstream(dir.path() / "Resources" / "GlobalLoad.txt") << "2,0,AAAAC,other_chr.s3d,x\n";
    for (const char* value : {"0", "FALSE", "", "1", "TRUE", "true"}) {
        write_ini(value);
        const auto list = global_character_archives(GameDir(dir.path()), true);
        const bool enabled = value[0] == '1' || value[0] == 'T' || value[0] == 't';
        CHECK(has(list, "global5_chr.s3d") == enabled);
        CHECK(has(list, "other_chr.s3d"));
    }
    std::ofstream(dir.path() / "eqclient.ini") << "[Defaults]\n";
    CHECK(has(global_character_archives(GameDir(dir.path())), "global5_chr.s3d")); // absent means on
    // Entries the player's GlobalLoad.txt names are kept with the option off (not confirmed either way).
    std::ofstream(dir.path() / "Resources" / "GlobalLoad.txt")
        << "2,0,AAAAC,global5_chr.s3d,x\n2,0,AAAAC,global5_chr2.s3d,x\n2,0,AAAAC,frog_mount_chr.s3d,x\n";
    write_ini("FALSE");
    const auto listed = global_character_archives(GameDir(dir.path()), true);
    CHECK(has(listed, "global5_chr.s3d"));
    CHECK(has(listed, "global5_chr2.s3d"));
    CHECK(has(listed, "frog_mount_chr.s3d"));
}

namespace {

bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) { return glm::length(a - b) < eps; }

// Two bones: a root at the origin and a child one unit up +X. The root turns
// 0 -> 90 degrees about Z over two frames.
CharacterModel two_bone_model() {
    CharacterModel m;
    m.code = "TST";
    m.bone_names = {"TST_TRACK", "TSTARM_TRACK"};
    m.parents = {-1, 0};
    m.order = {0, 1};
    AnimationClip rest;
    rest.tracks = {{BonePose{}}, {BonePose{glm::quat(1, 0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(1.0f)}}};
    m.clips.emplace("", rest);
    AnimationClip turn;
    turn.frame_ms = 100;
    turn.frame_count = 2;
    BonePose a, b;
    b.rotation = glm::angleAxis(std::numbers::pi_v<float> / 2.0f, glm::vec3(0, 0, 1));
    turn.tracks = {{a, b}, rest.tracks[1]};
    m.clips.emplace("L01", turn);
    return m;
}

} // namespace

TEST_CASE("CharacterModel composes parents and interpolates between frames") {
    auto m = two_bone_model();
    std::vector<glm::mat4> world;
    m.sample(*m.clip(""), 0.0f, true, world);
    REQUIRE(world.size() == 2);
    CHECK(near(glm::vec3(world[1] * glm::vec4(0, 0, 0, 1)), {1, 0, 0}));

    // Halfway between the frames (50 ms): the root has turned 45 degrees.
    m.sample(*m.clip("L01"), 0.05f, false, world);
    float h = std::sqrt(0.5f);
    CHECK(near(glm::vec3(world[1] * glm::vec4(0, 0, 0, 1)), {h, h, 0}));

    // Not looping: held at the last frame.
    m.sample(*m.clip("L01"), 5.0f, false, world);
    CHECK(near(glm::vec3(world[1] * glm::vec4(0, 0, 0, 1)), {0, 1, 0}));

    // Looping: 150 ms wraps from frame 1 back toward frame 0.
    m.sample(*m.clip("L01"), 0.15f, true, world);
    CHECK(near(glm::vec3(world[1] * glm::vec4(0, 0, 0, 1)), {h, h, 0}));

    CHECK(&m.first_clip({"L02", "L01"}) == m.clip("L01"));
    CHECK(&m.first_clip({"X99"}) == m.clip(""));
}

TEST_CASE("race model table") {
    const auto* hum = game::find_race_model(1, 0);
    REQUIRE(hum);
    CHECK(hum->code == "HUM");
    CHECK(game::find_race_model(1, 1)->code == "HUF");
    CHECK(game::find_race_model(12, 1)->code == "GNF");
    // Single-model NPC races are found whatever gender the server sends.
    CHECK(game::find_race_model(36, 0)->code == "RAT");
    CHECK(game::find_race_model(60, 2)->code == "SKE");
    CHECK((game::find_race_model(566, 0)->flags & game::kNewStyleModel) != 0);
    CHECK(game::find_race_model(35, 0) == nullptr); // not in the client's table
    CHECK(game::race_model_race_count() == 731);
    CHECK(game::animation_source("HUM") == "ELM");
    CHECK(game::animation_source("TRM") == "OGF");
    CHECK(game::animation_source("ELM").empty());
    // The RoF2 client's GetAlternateAnimTag for codes LanternExtractor's list leaves out.
    CHECK(game::animation_source("FRF") == "FRM");
    CHECK(game::animation_source("GHF") == "ELF");
    // With a TUNIC_POINT dag (the Luclin player models) humans, erudites and the skeleton take the human's clips and
    // a barbarian only its own; the other races still take the wood elf's.
    CHECK(game::animation_source("ERM", true) == "HUM");
    CHECK(game::animation_source("ERF", true) == "HUF");
    CHECK(game::animation_source("ERM") == "ELM");
    CHECK(game::animation_source("HUM", true).empty());
    CHECK(game::animation_source("BAF", true).empty());
    CHECK(game::animation_source("SKE", true) == "HUM");
    CHECK(game::animation_source("HIF", true) == "ELF");
    CHECK(game::animation_source("DAM", true) == "ELM");
}

TEST_CASE("the wood-elf prefix group borrows the wood elf's clips; codes the table names keep their donor") {
    // GetAlternateAnimTag @0x406a60: a code under one of its wood-elf prefixes takes the wood elf's two letters and
    // keeps its last one, the tunic flag aside, for the codes the table does not name: race 127's IVM among them.
    CHECK(game::animation_source("IVM") == "ELM");
    CHECK(game::animation_source("IVM", true) == "ELM");
    CHECK(game::animation_source("IVF") == "ELF");
    CHECK(game::animation_source("GEM") == "ELM");
    // Every table donor of a code under those prefixes, and of the codes the original names before them (ERO, BRI
    // and the like), is unchanged: the table is asked first. Apart from those early codes (BRI and HAG take other donors than the
    // rule would give), it agrees with the rule wherever both apply.
    const std::pair<std::string_view, std::string_view> kTableDonors[] = {
        {"BRF", "ELF"}, {"BRM", "ELM"}, {"DAF", "ELF"}, {"DAM", "ELM"}, {"EGM", "ELM"}, {"FEF", "ELF"},
        {"FEM", "ELM"}, {"FPM", "ELM"}, {"GFF", "ELF"}, {"GFM", "ELM"}, {"GHF", "ELF"}, {"GHM", "ELM"},
        {"HAF", "ELF"}, {"HAM", "ELM"}, {"HHM", "ELM"}, {"HIF", "ELF"}, {"HIM", "ELM"}, {"HLF", "ELF"},
        {"HLM", "ELM"}, {"NGM", "ELM"}, {"QCF", "ELF"}, {"QCM", "ELM"}, {"ZOF", "ELF"}, {"ZOM", "ELM"},
        {"ABH", "ELF"}, {"ERO", "ELF"}, {"HAG", "ELF"}, {"SIR", "ELF"}, {"BRI", "DWM"}, {"COK", "DWM"},
        {"BWD", "TRK"}, {"CCD", "TRK"}, {"DR2", "TRK"}, {"PRI", "TRK"}};
    for (const auto& [code, donor] : kTableDonors) {
        CAPTURE(code);
        CHECK(game::animation_source(code) == donor);
    }
    // Outside the group nothing new is lent.
    CHECK(game::animation_source("ELM").empty());
    CHECK(game::animation_source("WOL").empty());
}

TEST_CASE("the female froglok takes the male's clips: she sits instead of holding the rest pose" *
          doctest::skip(!hailstone::test::game_dir())) {
    // globalpcfroglok_chr.wld has tracks for FRM only (GetAlternateAnimTag @0x406a60: FRF -> FRM).
    const auto& game = *hailstone::test::game_dir();
    CharacterLibrary lib(CharacterLibrary::open_archives(game, {"globalpcfroglok_chr.s3d"}));
    auto m = lib.build("FRF");
    CHECK(m.animation_donor == "FRM");
    for (const char* code : {"P01", "P02", "P07", "O03", "L01"}) {
        const auto* c = m.clip(code);
        REQUIRE(c);
        CHECK(c->borrowed);
    }
}

TEST_CASE("classic human model from global_chr.s3d" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto archives = global_character_archives(game);
    CHECK(std::find(archives.begin(), archives.end(), "global_chr.s3d") != archives.end());

    CharacterLibrary lib(CharacterLibrary::open_archives(game, {"global_chr.s3d"}));
    CHECK(lib.model_codes().size() == 30); // skeletal actors (31 actor definitions)
    auto m = lib.build("HUM");
    CHECK(m.source == "global_chr.s3d:global_chr.wld");
    // Skeleton: 25 nodes, one root (HUM_TRACK), every parent before its child.
    REQUIRE(m.bone_names.size() == 25);
    CHECK(m.bone_names[0] == "HUM_TRACK");
    CHECK(m.bone_names[1] == "HUMPE_TRACK");
    CHECK(std::count(m.parents.begin(), m.parents.end(), -1) == 1);
    REQUIRE(m.order.size() == 25);
    for (std::size_t k = 0; k < m.order.size(); ++k) {
        auto b = m.order[k];
        if (m.parents[b] >= 0) {
            auto p = std::find(m.order.begin(), m.order.end(), static_cast<std::uint32_t>(m.parents[b]));
            CHECK(p < m.order.begin() + static_cast<std::ptrdiff_t>(k));
        }
    }
    // Body and head (HUM_DMSPRITEDEF + HUMHE00_DMSPRITEDEF).
    CHECK(m.vertices.size() == 791);
    CHECK(m.indices.size() == 444 * 3);
    for (const auto& v : m.vertices) CHECK(v.bones[0] < 25);
    // Rest pose: about 6.7 units tall, feet about 3 below the origin, front along +X.
    CHECK(m.bounds_min.z == doctest::Approx(-3.0f).epsilon(0.02));
    CHECK(m.bounds_max.z == doctest::Approx(3.67f).epsilon(0.02));

    // HUM has its own walk, run and stand tracks (24 of its 25 bones each);
    // the other 40 clips in global_chr come from ELM.
    CHECK(m.animation_donor == "ELM");
    CHECK(m.clips.size() == 44); // 43 clips + the rest pose
    for (const char* own : {"L01", "L02", "P01"}) {
        const auto* c = m.clip(own);
        REQUIRE(c);
        CHECK_FALSE(c->borrowed);
        CHECK(c->tracks.size() == 25);
        CHECK(c->frame_ms == 100);
    }
    auto animated = [&](const char* code) {
        const auto& t = m.clip(code)->tracks;
        return std::count_if(t.begin(), t.end(), [](const auto& keys) { return keys.size() > 1; });
    };
    CHECK(animated("L01") == 17);
    CHECK(animated("L02") == 17);
    CHECK(animated("P01") == 3);
    CHECK(m.clip("L01")->frame_count == 12);
    CHECK(m.clip("P01")->frame_count == 9);
    REQUIRE(m.clip("C01"));
    CHECK(m.clip("C01")->borrowed);
    CHECK(m.clip("T08")->frame_ms == 50);

    // A sampled walk frame stays a body: every vertex within 8 units of the origin.
    std::vector<glm::mat4> world;
    m.sample(*m.clip("L01"), 0.37f, true, world);
    for (const auto& v : m.vertices) CHECK(glm::length(glm::vec3(world[v.bones[0]] * glm::vec4(v.position, 1.0f))) < 8.0f);
}

TEST_CASE("every global character model builds" * doctest::skip(!hailstone::test::game_dir())) {
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "");
    std::size_t built = 0;
    for (const auto& code : lib.model_codes()) {
        // BOAT is a static actor without a mesh; IVM is now built as a meshless skeleton.
        if (code == "BOAT") continue;
        CHECK_NOTHROW(lib.build(code));
        ++built;
    }
    CHECK(built > 150);
}

TEST_CASE("Nektulos will-o-wisps (race 69) are the WIL actor, a glow with no light" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto* rm = game::find_race_model(69, 2);
    REQUIRE(rm);
    CHECK(rm->code == "WIL");
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "nektulos");
    auto wil = lib.model("WIL");
    REQUIRE(wil);
    // One additive (WILHE0001_MDF, raw 0x80000017) part: the original's wisp, not a sprite or particle effect.
    REQUIRE(wil->materials.size() == 1);
    CHECK(wil->materials[0].blend != BlendMode::Invisible);
    CHECK(!wil->parts.empty());
    CHECK(wil->clip("P01") != nullptr);
}

TEST_CASE("duplicate actor definitions: first at one level, latest across levels" *
          doctest::skip(!hailstone::test::game_dir())) {
    // BAM is defined in both global_chr and global4_chr.
    const auto& game = *hailstone::test::game_dir();
    auto archives = CharacterLibrary::open_archives(game, {"global_chr.s3d", "global4_chr.s3d"});
    REQUIRE(archives.size() == 2);
    CharacterLibrary same(archives, {2, 2});
    CharacterLibrary later(archives, {1, 2});
    REQUIRE(same.has_model("BAM"));
    CHECK(same.build("BAM").source.find("global_chr") != std::string::npos);
    CHECK(later.build("BAM").source.find("global4_chr") != std::string::npos);
}

TEST_CASE("global archives load at resource level 0" * doctest::skip(!hailstone::test::game_dir())) {
    // CDisplay::InitWorld: FLAGS[0] == 'T' -> level 0, else 2. Every NMS line starts with T.
    auto levels = global_archive_levels(*hailstone::test::game_dir());
    REQUIRE(!levels.empty());
    for (const auto& [name, level] : levels) CHECK(level == 0);
    CHECK(levels.contains("global_chr.s3d"));
    CHECK(kGlobalResourceLevel < kZoneResourceLevel);
}

TEST_CASE("a borrowed clip keeps the bones a Luclin model hides at rest hidden" * doctest::skip(!hailstone::test::game_dir())) {
    // The Luclin Erudite folds a robe flap away (ERMPEBUTTR scaled to 0.02) and borrows the Wood Elf's clips, whose
    // track for that bone has scale 1: unfolded, the flap reached out 13 units from the hip.
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "", true);
    auto m = lib.model("ERM");
    REQUIRE(m);
    auto clip = m->clips.find("P01");
    REQUIRE(clip != m->clips.end());
    CHECK(clip->second.borrowed);
    std::vector<glm::mat4> world;
    m->sample(clip->second, 0.3f, true, world);
    float reach = 0.0f;
    for (const auto& v : m->vertices)
        reach = std::max(reach, glm::length(glm::vec3(world[v.bones[0]] * glm::vec4(v.position, 1.0f))));
    CHECK(reach < 6.0f);
}

TEST_CASE("a Luclin erudite plays the human's clips" * doctest::skip(!hailstone::test::game_dir())) {
    // GetAlternateAnimTag with the tunic-point flag. With the wood elf's clips the erudite's face bones took the
    // elf's places (lips parted over the teeth, brows in a frown) and its beard point, which the elf has no track
    // for, stayed at its rest place in front of the mouth, so the beard hung there.
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "", true);
    auto m = lib.model("ERM");
    REQUIRE(m);
    CHECK(m->animation_donor == "HUM");
    auto clip = m->clips.find("P01");
    REQUIRE(clip != m->clips.end());
    REQUIRE(clip->second.borrowed);
    // The human's P01 keys the beard point on the head, as the hair point.
    int beard = m->find_bone("BEARD_POINT_TRACK");
    REQUIRE(beard >= 0);
    CHECK(glm::length(clip->second.tracks[static_cast<std::size_t>(beard)].front().translation) < 0.01f);
    // The jaw keeps the erudite's place, the human's being the same.
    int jaw = m->find_bone("FAJAW_TRACK");
    REQUIRE(jaw >= 0);
    CHECK(clip->second.tracks[static_cast<std::size_t>(jaw)].front().translation.y ==
          doctest::Approx(m->clips.at("").tracks[static_cast<std::size_t>(jaw)].front().translation.y).epsilon(0.01));
}

TEST_CASE("Luclin clips of more than 15 keys play at 50 ms, classic ones at 100" *
          doctest::skip(!hailstone::test::game_dir())) {
    // The Luclin dark elf female's C01 has 16 keys (the classic one 8): at 100 ms her swing took twice as long.
    auto luclin = CharacterLibrary::load(*hailstone::test::game_dir(), "", true);
    auto daf = luclin.model("DAF");
    REQUIRE(daf);
    if (daf->luclin) { // eqclient.ini may turn the Luclin dark elf female off
        REQUIRE(daf->clip("C01"));
        CHECK(daf->clip("C01")->frame_count > 15);
        CHECK(daf->clip("C01")->frame_ms == 50u);
    }
    // global_chr (TFFFC) is not a halving archive: the classic idle of 31 keys stays at 100 ms.
    auto classic = CharacterLibrary::load(*hailstone::test::game_dir(), "", false);
    auto old = classic.model("DAF");
    REQUIRE(old);
    REQUIRE_FALSE(old->luclin);
    REQUIRE(old->clip("O01"));
    CHECK(old->clip("O01")->frame_count > 15);
    CHECK(old->clip("O01")->frame_ms == 100u);
}

TEST_CASE("a Luclin model borrows nothing from the classic donor's skeleton" * doctest::skip(!hailstone::test::game_dir())) {
    // global_chr's classic ELF has C10, C11 and T07..T09, the Luclin ELF has not; the few bone names the two share
    // lent a Luclin human those clips with every other bone in the rest pose: a T-pose.
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "", true);
    for (const char* code : {"HUF", "HUM", "HIF", "ERM"}) {
        CAPTURE(code);
        auto m = lib.model(code);
        REQUIRE(m);
        REQUIRE(m->source.find("global_chr.s3d") == std::string::npos); // the Luclin model
        for (const char* clip : {"C10", "C11", "T07", "T08", "T09"}) {
            CAPTURE(clip);
            const auto* c = m->clip(clip);
            // Its own clip, or none; never a one-frame borrowed one.
            if (c) CHECK((!c->borrowed || c->frame_count > 1));
        }
        // What the Luclin donor has is still lent.
        const auto* p01 = m->clip("P01");
        REQUIRE(p01);
        CHECK(p01->frame_count > 1);
    }
}

TEST_CASE("a spawn holding something plays the model's variant B, else what it has (0x4b1210)") {
    CharacterModel m;
    for (const char* c : {"", "P01", "P01B", "C01"}) m.clips[c].frame_count = 2;
    m.held_aliases["P01"] = "P01B";
    CHECK(m.clip("P01", true) == &m.clips.at("P01B"));
    CHECK(m.clip("P01", false) == &m.clips.at("P01"));
    // No variant B: column 0's clip.
    CHECK(m.clip("C01", true) == &m.clips.at("C01"));
    CHECK(m.clip("C02", true) == nullptr);
    CHECK(&m.first_clip({"P01"}, true) == &m.clips.at("P01B"));
    // PlayerClient_UpdateHoldingAnimation: either hand's model sets the column.
    hailstone::game::Appearance a;
    CHECK_FALSE(hailstone::game::holds_item(a));
    a.material[hailstone::game::kSlotSecondary] = 101064;
    CHECK(hailstone::game::holds_item(a));
    a.material[hailstone::game::kSlotSecondary] = 0;
    a.material[hailstone::game::kSlotPrimary] = 10;
    CHECK(hailstone::game::holds_item(a));
}

TEST_CASE("a Luclin model holding something raises its shield arm (variant B)" * doctest::skip(!hailstone::test::game_dir())) {
    // InitAnimationTracks binds the tunic-point models' A..H tracks to holding columns 0..7; the Dark Elf female
    // holding a sword and a shield stands in P01B, whose shield point is well above P01A's, where the arm hangs.
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "", true);
    auto m = lib.model("DAF");
    REQUIRE(m);
    REQUIRE(m->find_bone("TUNIC_POINT_TRACK") >= 0);
    const auto* held = m->clip("P01", true);
    const auto* empty = m->clip("P01", false);
    REQUIRE(held);
    REQUIRE(empty);
    CHECK(held == m->clip("P01B"));
    CHECK(held != empty);
    const int shield = m->find_bone("SHIELD_POINT_TRACK");
    REQUIRE(shield >= 0);
    std::vector<glm::mat4> a, b;
    m->sample(*empty, 0.0f, true, a);
    m->sample(*held, 0.0f, true, b);
    CHECK(b[static_cast<std::size_t>(shield)][3].z > a[static_cast<std::size_t>(shield)][3].z + 0.25f);
}

TEST_CASE("zone_character_archives: RoF2's and the live client's _chr.txt entries") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
                   ("hailstone_chr_txt_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    auto touch = [&](const char* name, const char* text = "") { std::ofstream(dir / name) << text; };
    // RoF2: "kob,kob" is kob.eqg, "rtn,rtn" an EQG too, "skh,qeynos_chr2" an S3D. Live: "cln,cln_chr" is cln_chr.s3d.
    touch("zone_chr.txt", "4\nkob,kob\nskh,qeynos_chr2\ncln,cln_chr\nrtn,rtn\n");
    touch("kob.eqg");
    touch("qeynos_chr2.s3d");
    touch("cln_chr.s3d");
    GameDir game(dir);
    CHECK(zone_character_archives(game, "zone") == std::vector<std::string>{"qeynos_chr2.s3d", "cln_chr.s3d"});
    CHECK_FALSE(chr_txt_entry_is_s3d(game, "kob", "kob"));
    CHECK(chr_txt_entry_is_s3d(game, "CLN", "Cln_Chr"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("live hero's forge pieces: Resources/Layers rows name a base mesh and a texture layer") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
                   ("hailstone_layers_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "Resources" / "Layers");
    auto touch = [&](const char* name, const char* text) { std::ofstream(dir / name, std::ios::binary) << text; };
    // The files read in name order, the first row for a piece winning; CRLF lines.
    touch("Resources/Layers/B_IT.txt", "IT27104^IT11111^OTHER\r\n");
    touch("Resources/Layers/GENERIC_CHAIN_BLUE_V1_IT.txt",
          "IT27000^IT14400^GENERIC_CHAIN_BLUE_V1\r\nit27104^it19814^generic_chain_blue_v1\r\nbad line\r\n");
    EqgCharacterStore store{GameDir(dir)};
    CHECK(store.layered_part("IT27104") == std::pair<std::string, std::string>{"IT11111", "OTHER"});
    CHECK(store.layered_part("it27000") == std::pair<std::string, std::string>{"IT14400", "GENERIC_CHAIN_BLUE_V1"});
    CHECK_FALSE(store.layered_part("IT1"));
    // Neither base mesh nor layer exists here.
    CHECK(store.part_mesh("IT27104") == nullptr);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("hero's forge layered parts: a long chain of distinct names stops at kMaxLayerDepth") {
    // P0 on P1 on P2 ... with no mesh anywhere: each link used to recurse once, overflowing the stack.
    test::ScratchDir dir("layers_chain");
    std::filesystem::create_directories(dir.path() / "Resources" / "Layers");
    {
        std::ofstream out(dir.path() / "Resources" / "Layers" / "chain.txt", std::ios::binary);
        for (int i = 0; i < 200000; ++i) out << "P" << i << "^P" << i + 1 << "^L\r\n";
    }
    EqgCharacterStore store{GameDir(dir.path())};
    CHECK(store.layered_part("P0") == std::pair<std::string, std::string>{"P1", "L"});
    CHECK(store.part_mesh("P0") == nullptr);
    CHECK(store.part_mesh("P5") == nullptr); // cached on the way down
}

namespace {

// Authored fixtures contain only a triangle and invented material names.
std::vector<std::byte> armor_fixture_mesh(float marker, bool parameters = true,
                                         std::string_view shader = "Opaque_MaxCBSGE1.fx", bool coverage = false) {
    ByteWriter strings, materials;
    auto str = [&](std::string_view value) {
        auto offset = static_cast<std::uint32_t>(strings.size());
        strings.fixed_string(value, value.size() + 1);
        return offset;
    };
    for (std::uint32_t i = 0; i < 2; ++i) {
        materials.le(i);
        materials.le(str("M" + std::to_string(i)));
        materials.le(str(shader));
        materials.le<std::uint32_t>((parameters ? 6u : 1u) + (coverage ? 2u : 0u));
        for (const auto& [key, value] : std::array{
                 std::pair{"e_TextureDiffuse0", "BASE_C.DDS"}, std::pair{"e_TextureNormal0", "BASE_N.DDS"},
                 std::pair{"e_TextureEnvironment0", "BASE_E.DDS"}}) {
            if (!parameters && std::string_view(key) != "e_TextureDiffuse0") continue;
            materials.le(str(key));
            materials.le<std::uint32_t>(2);
            materials.le(str(value));
        }
        for (const auto& [key, value] : std::array{
                 std::pair{"e_fShininess0", 24.0f}, std::pair{"e_fGloss0", 0.7f},
                 std::pair{"e_fEnvMapStrength0", 0.25f}}) {
            if (!parameters) continue;
            materials.le(str(key));
            materials.le<std::uint32_t>(0);
            materials.f32(value);
        }
        if (coverage) {
            materials.le(str("e_TextureCoverage0"));
            materials.le<std::uint32_t>(2);
            materials.le(str("COVERAGE.DDS"));
            materials.le(str("e_fCoverageScale0"));
            materials.le<std::uint32_t>(0);
            materials.f32(2.5f);
        }
    }
    ByteWriter out;
    out.fixed_string("EQGM", 4);
    for (std::uint32_t v : {3u, static_cast<std::uint32_t>(strings.size()), 2u, 3u, 2u, 0u}) out.le(v);
    out.bytes(strings.data());
    out.bytes(materials.data());
    for (int i = 0; i < 3; ++i) {
        for (float v : {marker + static_cast<float>(i), 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}) out.f32(v);
        out.le<std::uint32_t>(0xffffffffu);
        for (int j = 0; j < 4; ++j) out.f32(0.0f);
    }
    for (std::uint32_t material = 0; material < 2; ++material)
        for (std::uint32_t v : {0u, 1u, 2u, material, 0u}) out.le(v);
    return out.take();
}

std::vector<std::byte> armor_fixture_layer(bool maps = true, bool color = true) {
    ByteWriter strings;
    auto str = [&](std::string_view value) {
        auto offset = static_cast<std::uint32_t>(strings.size());
        strings.fixed_string(value, value.size() + 1);
        return offset;
    };
    auto name = str("SURFACE"), diffuse = str("SURFACE_C.DDS"), normal = str("SURFACE_N.DDS"),
         environment = str("SURFACE_E.DDS");
    ByteWriter out;
    out.fixed_string("EQGL", 4);
    for (std::uint32_t v : {4u, static_cast<std::uint32_t>(strings.size()), 1u}) out.le(v);
    out.bytes(strings.data());
    for (std::uint32_t v : {name, color ? diffuse : 0xffffffffu, maps ? normal : 0xffffffffu,
                           maps ? environment : 0xffffffffu,
                           0xffffffffu, 0xffffffffu}) out.le(v);
    out.f32(12.0f);
    out.f32(0.5f);
    return out.take();
}

void write_armor_fixture(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void armor_fixture_folder(const std::filesystem::path& dir, bool maps = true, bool diffuse = true,
                          bool parameters = true, bool color = true) {
    std::filesystem::create_directories(dir / "Resources" / "Layers");
    std::ofstream(dir / "Resources" / "OnDemandResources.txt")
        << "direct.eqg^IT1.MOD^IT1_ACTORDEF^EQGM\nbase.eqg^IT2.MOD^IT2_ACTORDEF^EQGM\n"
           "direct.eqg^IT1.MOD^IT3_ACTORDEF^EQGM\ndirect.eqg^IT1.MOD^IT4_ACTORDEF^EQGM\n"
           "direct.eqg^IT9.MOD^IT9_ACTORDEF^EQGM\n"
           "surface.eqg^surface.lay^SURFACE^EQGL\n";
    std::ofstream(dir / "Resources" / "Layers" / "A.txt")
        << "\xef\xbb\xbf IT1 ^ IT2 ^ SURFACE \r\nIT2^IT999^SURFACE\nIT3^IT999^SURFACE\nIT4^IT2^MISSING\n"
           "IT5^IT6^SURFACE\nIT6^IT5^SURFACE\nIT-1^IT2^SURFACE\nIT7^../IT2^SURFACE\n"
           "IT8^IT2^SURFACE^EXTRA\n";
    std::ofstream(dir / "Resources" / "Layers" / "C.txt") << "IT9^IT2^MISSING\n";
    std::ofstream(dir / "Resources" / "Layers" / "B.txt") << "IT1^IT999^OTHER\n";
    auto incompatible = armor_fixture_mesh(30.0f, parameters);
    incompatible[20] = std::byte{1}; // one triangle instead of the base's two
    incompatible.resize(incompatible.size() - 20);
    write_armor_fixture(dir / "direct.eqg", test::pfs_fixtures::make_pfs({
        {"it1.mod", armor_fixture_mesh(10.0f, parameters)}, {"it9.mod", std::move(incompatible)}}));
    write_armor_fixture(dir / "base.eqg", test::pfs_fixtures::make_pfs({{"it2.mod", armor_fixture_mesh(20.0f, parameters)}}));
    std::vector<std::pair<std::string, std::vector<std::byte>>> files{
        {"surface.lay", armor_fixture_layer(maps, color)}, {"surface_n.dds", {std::byte{1}}},
        {"surface_e.dds", {std::byte{1}}}};
    if (diffuse) files.push_back({"surface_c.dds", {std::byte{1}}});
    write_armor_fixture(dir / "surface.eqg", test::pfs_fixtures::make_pfs(files));
}

} // namespace

TEST_CASE("a held item with a 0x2d mesh keeps it when its skeleton has no mesh of its own") {
    using namespace test::wld_fixtures;
    WldBuilder b;
    auto bitmap = b.add(0x03, b.str("A_SPRITE"), bitmap_name("fixture.bmp"));
    ByteWriter info;
    info.le<std::uint32_t>(0);
    info.le<std::uint32_t>(1);
    info.le(bitmap);
    auto sprite_info = b.add(0x04, 0, info);
    ByteWriter ref;
    ref.le(sprite_info);
    ref.le<std::uint32_t>(0);
    auto sprite = b.add(0x05, 0, ref);
    auto mat = b.add(0x30, b.str("A_MDF"), material(0x80000001, sprite));
    auto materials = b.add(0x31, 0, refs({mat, mat, mat}));
    auto geometry = b.add(0x36, b.str("IT9_DMSPRITEDEF"), mesh(materials, false));
    ref = {};
    ref.le(geometry);
    ref.le<std::uint32_t>(0);
    auto mesh_ref = b.add(0x2d, 0, ref);
    ByteWriter track;
    track.le<std::int32_t>(0);
    track.le<std::uint32_t>(0);
    auto root_track = b.add(0x13, b.str("IT9_ROOT_TRACK"), track);
    // A skeleton of one node with no mesh and no cloud now builds as an empty model: the static 0x2d mesh
    // of the item is still the one kept.
    ByteWriter skeleton;
    skeleton.le<std::uint32_t>(0);
    skeleton.le<std::uint32_t>(1);
    skeleton.le<std::int32_t>(0);
    skeleton.le(b.str("IT9_ROOT_DAG"));
    skeleton.le<std::uint32_t>(0);
    skeleton.le(root_track);
    skeleton.le<std::int32_t>(0);
    skeleton.le<std::uint32_t>(0);
    auto skeleton_def = b.add(0x10, b.str("IT9_HS_DEF"), skeleton);
    ref = {};
    ref.le(skeleton_def);
    ref.le<std::uint32_t>(0);
    auto skeleton_ref = b.add(0x11, 0, ref);
    ByteWriter actor;
    for (std::uint32_t v : {0u, 0u, 0u, 2u, 0u}) actor.le(v);
    actor.le(mesh_ref);
    actor.le(skeleton_ref);
    b.add(0x14, b.str("IT9_ACTORDEF"), actor);
    auto archive = std::make_shared<CharacterArchive>();
    archive->archive = std::make_shared<PfsArchive>(PfsArchive::from_memory(
        test::pfs_fixtures::make_pfs({{"fixture.wld", b.build()}}), "held_fixture.s3d"));
    archive->wlds.push_back(wld::WldFile::parse(*archive->archive->read("fixture.wld"), "fixture.wld"));
    CharacterLibrary library({archive});
    CHECK_NOTHROW(library.build("IT9"));
    auto item = library.actor_mesh("IT9");
    REQUIRE(item);
    CHECK_FALSE(item->mesh.indices.empty());
    CHECK(item->mesh.emitters.empty());
}

TEST_CASE("a skeleton without a mesh loads and dresses its held item, but shows nothing bare") {
    using namespace test::wld_fixtures;
    // Fixture facts: item IT9 has the static 0x2d fixture mesh. The skeleton has a root with the R_POINT and L_POINT
    // bones under it, and no clips: the wood elf it borrows from (ELM) is not in this WLD.
    constexpr std::uint32_t kItemModel = 9;
    const std::string item_code = "IT" + std::to_string(kItemModel);
    constexpr std::size_t kRootBone = 0, kRightHandBone = 1, kLeftHandBone = 2;
    constexpr std::array<std::uint32_t, 2> kHandBones{kRightHandBone, kLeftHandBone};
    constexpr std::size_t kSkeletonNodes = 1 + kHandBones.size();
    constexpr std::array kHeldSlots{game::kSlotPrimary, game::kSlotSecondary};
    WldBuilder b;
    auto bitmap = b.add(kBitmapName, b.str("A_SPRITE"), bitmap_name("fixture.bmp"));
    const std::array bitmaps{bitmap};
    ByteWriter info;
    info.le<std::uint32_t>(0); // flags
    info.le(static_cast<std::uint32_t>(bitmaps.size()));
    for (auto bm : bitmaps) info.le(bm);
    auto sprite_info = b.add(kBitmapInfo, 0, info);
    // A reference fragment: the referenced fragment, then its flags (none).
    auto reference = [](std::int32_t to) {
        ByteWriter w;
        w.le(to);
        w.le<std::uint32_t>(0);
        return w;
    };
    auto sprite = b.add(kSimpleSprite, 0, reference(sprite_info));
    auto mat = b.add(kMaterialDef, b.str("A_MDF"), material(kOpaqueRenderMethod, sprite));
    auto materials = b.add(kMaterialPalette, 0, refs({mat, mat, mat}));
    auto item_geometry = b.add(kMeshDef, b.str(item_code + "_DMSPRITEDEF"), mesh(materials, false));
    auto item_mesh_ref = b.add(kMeshRef, 0, reference(item_geometry));

    // A 0x10 skeleton without a mesh list: flags, node count, collision volume, then per node its name, flags,
    // track, sprite (none: no body mesh) and child list.
    auto skeleton_def = [&](const std::vector<std::string>& dags, const std::vector<std::int32_t>& tracks,
                            const std::vector<std::vector<std::uint32_t>>& children) {
        ByteWriter w;
        w.le<std::uint32_t>(0);
        w.le(static_cast<std::uint32_t>(dags.size()));
        w.le<std::int32_t>(0);
        for (std::size_t i = 0; i < dags.size(); ++i) {
            w.le(b.str(dags[i]));
            w.le<std::uint32_t>(0);
            w.le(tracks[i]);
            w.le<std::int32_t>(0);
            w.le(static_cast<std::uint32_t>(children[i].size()));
            for (std::uint32_t c : children[i]) w.le(c);
        }
        return w;
    };
    // A 0x14 actor definition with no actions: flags, callback, action count, sprite count, bounds reference, then
    // the sprite references.
    auto actor_def = [](std::initializer_list<std::int32_t> sprites) {
        ByteWriter w;
        w.le<std::uint32_t>(0);
        w.le<std::int32_t>(0);
        w.le<std::uint32_t>(0);
        w.le(static_cast<std::uint32_t>(sprites.size()));
        w.le<std::int32_t>(0);
        for (std::int32_t sp : sprites) w.le(sp);
        return w;
    };
    // A one-key track reference: the rest pose of a bone with no track definition of its own.
    auto rest_track = reference(0);
    auto item_root_track = b.add(kTrackRef, b.str(item_code + "_ROOT_TRACK"), rest_track);
    auto item_skeleton =
        b.add(kSkeletonDef, b.str(item_code + "_HS_DEF"), skeleton_def({item_code + "_ROOT_DAG"}, {item_root_track}, {{}}));
    auto item_skeleton_ref = b.add(kSkeletonRef, 0, reference(item_skeleton));
    b.add(kActorDef, b.str(item_code + "_ACTORDEF"), actor_def({item_mesh_ref, item_skeleton_ref}));

    // Race 127's IVM: bones naming the hand attach points, no node sprite and no mesh list.
    auto root_track = b.add(kTrackRef, b.str("IVM_ROOT_TRACK"), rest_track);
    auto left_point_track = b.add(kTrackRef, b.str("IVML_POINT_TRACK"), rest_track);
    auto r_point_track = b.add(kTrackRef, b.str("IVMR_POINT_TRACK"), rest_track);
    std::vector<std::string> dags(kSkeletonNodes);
    std::vector<std::int32_t> tracks(kSkeletonNodes);
    std::vector<std::vector<std::uint32_t>> children(kSkeletonNodes);
    dags[kRootBone] = "IVM_ROOT_DAG";
    dags[kRightHandBone] = "IVMR_POINT_DAG";
    dags[kLeftHandBone] = "IVML_POINT_DAG";
    tracks[kRootBone] = root_track;
    tracks[kRightHandBone] = r_point_track;
    tracks[kLeftHandBone] = left_point_track;
    children[kRootBone].assign(kHandBones.begin(), kHandBones.end());
    auto skeleton = b.add(kSkeletonDef, b.str("IVM_HS_DEF"), skeleton_def(dags, tracks, children));
    auto skeleton_ref = b.add(kSkeletonRef, 0, reference(skeleton));
    b.add(kActorDef, b.str("IVM_ACTORDEF"), actor_def({skeleton_ref}));

    // A static actor without a mesh is still a build error.
    b.add(kActorDef, b.str("BOAT_ACTORDEF"), actor_def({0}));

    auto archive = std::make_shared<CharacterArchive>();
    archive->archive = std::make_shared<PfsArchive>(PfsArchive::from_memory(
        test::pfs_fixtures::make_pfs({{"fixture.wld", b.build()}}), "meshless_fixture.s3d"));
    archive->wlds.push_back(wld::WldFile::parse(*archive->archive->read("fixture.wld"), "fixture.wld"));
    CharacterLibrary library({archive});
    auto item = library.actor_mesh(item_code);
    REQUIRE(item);
    const std::size_t item_vertices = item->mesh.vertices.size();
    REQUIRE(item_vertices > 0);

    auto model = library.model("IVM");
    REQUIRE(model);
    CHECK(bodiless_skeleton(*model));
    CHECK(model->vertices.empty());
    CHECK(model->indices.empty());
    CHECK(model->clouds.empty());
    REQUIRE(model->parents.size() == kSkeletonNodes);
    for (auto hand : kHandBones) CHECK(model->parents[hand] == static_cast<std::int32_t>(kRootBone));
    CHECK(model->find_bone_prefix("R_POINT") == static_cast<int>(kRightHandBone));
    CHECK(model->find_bone_prefix("L_POINT") == static_cast<int>(kLeftHandBone));
    CHECK(model->animation_donor == "ELM");
    REQUIRE(model->clip(""));
    CHECK_THROWS(library.build("BOAT"));

    game::Appearance a;
    auto bare = library.dress(*model, a);
    REQUIRE(bare);
    CHECK(bare->vertices.empty());
    CHECK(bare->indices.empty());
    CHECK(bare->parts.empty()); // no draw parts for a bare meshless skeleton
    CHECK(bare->held_reach == 0.0f);
    glm::vec3 lo, hi;
    CHECK_FALSE(dressed_bounds(*model, *bare, lo, hi));

    a.material[kHeldSlots.front()] = kItemModel;
    auto armed = library.dress(*model, a);
    REQUIRE(armed);
    REQUIRE_FALSE(armed->indices.empty());
    CHECK(armed->vertices.size() == item_vertices);
    REQUIRE_FALSE(armed->parts.empty());
    CHECK(armed->parts.front().material < armed->materials.size());
    for (const auto& v : armed->vertices) CHECK(v.bones[0] == kRightHandBone);
    REQUIRE(dressed_bounds(*model, *armed, lo, hi));
    CHECK(lo.x < hi.x); // the item gives the empty body usable bounds
    // Its reach, found once when dressed, holds every item vertex where the rest clip puts it.
    std::vector<glm::mat4> world;
    model->sample(*model->clip(""), 0.0f, false, world);
    for (const auto& v : armed->vertices)
        CHECK(armed->held_reach >= glm::length(glm::vec3(world[v.bones[0]] * glm::vec4(v.position, 1.0f))));

    for (auto slot : kHeldSlots) a.material[slot] = kItemModel;
    auto both = library.dress(*model, a);
    REQUIRE(both);
    CHECK(both->vertices.size() == kHeldSlots.size() * item_vertices);
    CHECK(both->parts.size() >= kHeldSlots.size());
    for (auto hand : kHandBones)
        CHECK(static_cast<std::size_t>(std::count_if(both->vertices.begin(), both->vertices.end(), [hand](const auto& v) {
                  return v.bones[0] == hand;
              })) == item_vertices);
}

TEST_CASE("real IVM from the game folder loads with no body mesh and dresses IT10649" *
          doctest::skip(!hailstone::test::game_dir())) {
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "");
    auto model = lib.model("IVM");
    REQUIRE(model);
    // Its 0x2d mesh references name 0x2c fragments (IVM_DMSPRITEDEF), which this client does not support: no body.
    CHECK(model->vertices.empty());
    CHECK(model->indices.empty());
    // chrdump IVM: the right hand point's chain up to the root.
    const std::array<std::string_view, 8> kHandChain{"IVMR_POINT_TRACK", "IVMFI_R2_TRACK", "IVMFI_R_TRACK",
                                                      "IVMFO_R_TRACK",    "IVMBI_R_TRACK",  "IVMCH_TRACK",
                                                      "IVMPE_TRACK",      "IVM_TRACK"};
    const int hand = model->find_bone_prefix("R_POINT");
    REQUIRE(hand >= 0);
    std::int32_t at = hand;
    for (std::size_t i = 0; i < kHandChain.size(); ++i) {
        CAPTURE(i);
        REQUIRE(at >= 0);
        REQUIRE(static_cast<std::size_t>(at) < model->bone_names.size());
        std::string name = model->bone_names[static_cast<std::size_t>(at)];
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        CHECK(name == kHandChain[i]);
        at = model->parents[static_cast<std::size_t>(at)];
    }
    CHECK(at < 0); // the chain ends at the root
    // It plays the wood elf's clips (GetAlternateAnimTag @0x406a60): in ELM's idle its hands are where an elf's
    // are, close together at its sides, not spread as in its own T-shaped rest pose.
    CHECK(model->animation_donor == "ELM");
    auto elf = lib.model("ELM");
    REQUIRE(elf);
    const auto hands_apart = [](const CharacterModel& m, std::string_view clip) {
        const int right = m.find_bone_prefix("R_POINT"), left = m.find_bone_prefix("L_POINT");
        REQUIRE(right >= 0);
        REQUIRE(left >= 0);
        const AnimationClip* c = m.clip(clip);
        REQUIRE(c);
        std::vector<glm::mat4> world;
        m.sample(*c, 0.0f, false, world);
        return glm::distance(glm::vec3(world[static_cast<std::size_t>(right)][3]),
                             glm::vec3(world[static_cast<std::size_t>(left)][3]));
    };
    const float idle = hands_apart(*model, "P01");
    CHECK(idle == doctest::Approx(hands_apart(*elf, "P01")));
    CHECK(idle < hands_apart(*model, ""));
    constexpr std::uint32_t kHeldItem = 10649;
    REQUIRE(lib.has_item(kHeldItem));

    game::Appearance a;
    a.material[game::kSlotPrimary] = kHeldItem;
    auto armed = lib.dress(*model, a);
    REQUIRE(armed);
    REQUIRE_FALSE(armed->indices.empty());
    CHECK(std::any_of(armed->vertices.begin(), armed->vertices.end(),
                      [hand](const auto& v) { return v.bones[0] == hand; }));
    glm::vec3 lo, hi;
    REQUIRE(dressed_bounds(*model, *armed, lo, hi));
    CHECK(glm::length(glm::max(glm::abs(lo), glm::abs(hi))) > 0.0f);
}

TEST_CASE("held EQG items keep native surface maps without taking armor aliases") {
    test::ScratchDir dir("held_surface");
    armor_fixture_folder(dir.path());
    auto lib = CharacterLibrary::load(GameDir(dir.path()), "");
    auto item = lib.item_mesh(1);
    REQUIRE(item);
    REQUIRE(item->mesh.materials.size() == 2);
    CHECK(item->mesh.vertices[0].position.x == 10.0f); // direct held item, not the IT2 armor alias
    CHECK(item->mesh.materials[0].effect.normal_map == "base_n.dds");
    CHECK(item->mesh.materials[0].effect.textures == std::vector<std::string>{"base_e.dds"});
    CHECK(item->mesh.materials[0].effect.shininess == 24.0f);
    // EQG effect_params uses twice the authored environment strength, matching its native surface formula.
    CHECK(item->mesh.materials[0].effect.env_strength == doctest::Approx(0.5f));
}

TEST_CASE("MPL item coverage survives material parsing and surface attachment without changing ordinary bodies") {
    for (std::string_view shader : {"Opaque_MPLBasic.fx", "Opaque_MPLBump.fx", "Chroma_MPLBumpAT.fx",
                                    "Alpha_MPLBumpA.fx"}) {
        CAPTURE(shader);
        const auto source = eqg_character::parse_mesh(armor_fixture_mesh(0.0f, true, shader, true), "fixture");
        REQUIRE(source.materials.size() == 2);
        CHECK(source.materials[0].effect.coverage_map == "coverage.dds");
        CHECK(source.materials[0].effect.coverage_scale == doctest::Approx(2.5f));
        CHECK_FALSE(source.materials[0].effect.packed_normal);
        CharacterMesh attachment, body;
        eqg_character::append_pieces(source, {0}, attachment, true);
        eqg_character::append_pieces(source, {0}, body);
        REQUIRE(attachment.materials.size() == 2);
        CHECK(attachment.materials[0].effect.coverage_map == "coverage.dds");
        CHECK(attachment.materials[0].effect.coverage_scale == doctest::Approx(2.5f));
        CHECK(body.materials[0].effect.coverage_map.empty());
        CHECK(body.materials[0].effect.coverage_scale == 1.0f);
    }
    const auto non_mpl = eqg_character::parse_mesh(
        armor_fixture_mesh(0.0f, true, "Opaque_MaxCB1.fx", true), "fixture");
    CHECK(non_mpl.materials[0].effect.coverage_map.empty());
    CHECK(non_mpl.materials[0].effect.coverage_scale == 1.0f);
    eqg::MaterialDef missing;
    missing.shader = "Opaque_MPLBasic.fx";
    CHECK(eqg::effect_params(missing, EqgShader::None).coverage_map.empty());
    CHECK(eqg::effect_params(missing, EqgShader::None).coverage_scale == 1.0f);
    missing.properties.push_back({"e_fCoverageScale0", 0, std::numeric_limits<float>::quiet_NaN(), 0, {}});
    CHECK(eqg::effect_params(missing, EqgShader::None).coverage_scale == 1.0f);
}

TEST_CASE("MPL coverage identity distinguishes attachment maps and their UV scale") {
    const auto original = eqg_character::parse_mesh(
        armor_fixture_mesh(0.0f, true, "Opaque_MPLBump.fx", true), "fixture");
    for (bool change_scale : {false, true}) {
        auto source = original;
        source.materials[0].name = source.materials[1].name = "SHARED";
        if (change_scale) source.materials[1].effect.coverage_scale = 4.0f;
        else source.materials[1].effect.coverage_map = "other_coverage.dds";
        CharacterMesh attachment;
        eqg_character::append_pieces(source, {0}, attachment, true);
        REQUIRE(attachment.materials.size() == 2);
        CHECK(attachment.parts[0].material != attachment.parts[1].material);
    }
}

TEST_CASE("Scrolling held surfaces keep distinct authored UV velocities when their names and textures match") {
    auto source = eqg_character::parse_mesh(armor_fixture_mesh(0.0f, true, "Opaque_MaxWaterFall.fx"), "fixture");
    REQUIRE(source.materials.size() == 2);
    source.materials[0].name = source.materials[1].name = "SHARED";
    source.materials[0].effect.slide = {0.0f, 0.5f, 0.25f, 0.0f};
    source.materials[1].effect.slide = {0.0f, -0.5f, -0.25f, 0.0f};
    CharacterMesh mesh;
    eqg_character::append_pieces(source, {0}, mesh, true);
    REQUIRE(mesh.materials.size() == 2);
    CHECK(mesh.materials[0].eqg_shader == EqgShader::WaterFall);
    CHECK(mesh.materials[1].eqg_shader == EqgShader::WaterFall);
    CHECK(mesh.materials[0].effect.slide == source.materials[0].effect.slide);
    CHECK(mesh.materials[1].effect.slide == source.materials[1].effect.slide);
    CHECK(mesh.parts[0].material != mesh.parts[1].material);
}

TEST_CASE("hero's forge armor aliases precede colliding generic parts without sharing their cache") {
    test::ScratchDir dir("armor_alias");
    armor_fixture_folder(dir.path());
    for (bool armor_first : {false, true}) {
        EqgCharacterStore store{GameDir(dir.path())};
        if (armor_first) REQUIRE(store.armor_part_mesh("IT1"));
        auto direct = store.part_mesh("IT1");
        auto armor = store.armor_part_mesh("it1");
        REQUIRE(direct);
        REQUIRE(armor);
        CHECK(direct->pieces[0].vertices[0].position.x == 10.0f);
        CHECK(armor.mesh->pieces[0].vertices[0].position.x == 20.0f);
        CHECK(store.armor_part_mesh("IT1").mesh == armor.mesh);
        CHECK(store.part_mesh("IT1") == direct);
        CHECK(armor.surface_maps);
        REQUIRE(armor.mesh->materials.size() == 2);
        for (const auto& material : armor.mesh->materials) {
            CHECK(material.diffuse == "surface_c.dds");
            CHECK(material.effect.normal_map == "surface_n.dds");
            CHECK(material.effect.textures == std::vector<std::string>{"surface_e.dds"});
            CHECK(material.effect.shininess == 12.0f);
            CHECK(material.effect.gloss == doctest::Approx(0.7f));
            CHECK(material.effect.env_strength == 0.5f);
            CHECK(material.effect.specular);
            CHECK(material.effect.reflective);
        }
        CharacterMesh plain_mesh, armor_mesh;
        eqg_character::append_pieces(*direct, {0}, plain_mesh);
        eqg_character::append_pieces(*armor.mesh, {0}, armor_mesh, armor.surface_maps);
        REQUIRE(plain_mesh.materials.size() == 2);
        REQUIRE(armor_mesh.materials.size() == 2);
        CHECK(plain_mesh.materials[0].effect.normal_map.empty());
        CHECK_FALSE(plain_mesh.materials[0].effect.specular);
        CHECK(armor_mesh.materials[0].effect.normal_map == "surface_n.dds");
        CHECK(armor_mesh.materials[1].effect.textures == std::vector<std::string>{"surface_e.dds"});
        // A row whose base is missing is not followed: the direct part, as with no row, plainly lit.
        auto unmapped = store.armor_part_mesh("IT3");
        REQUIRE(unmapped);
        CHECK(unmapped.mesh == store.part_mesh("IT3"));
        CHECK_FALSE(unmapped.surface_maps);
        CHECK_FALSE(store.armor_alias("IT3"));
        CHECK(store.armor_alias("IT1"));
        auto legacy = store.armor_part_mesh("IT4"); // an existing base with an absent layer keeps its direct look
        REQUIRE(legacy);
        CHECK(legacy.mesh->pieces[0].vertices[0].position.x == 10.0f);
        CHECK(legacy.mesh == store.part_mesh("IT4")); // shared, not copied
        CHECK_FALSE(legacy.surface_maps);
        CHECK(store.part_mesh("IT3"));
        CHECK(store.part_mesh("IT4"));
        CHECK_FALSE(store.armor_part_mesh("IT5")); // cycle: no base, no direct part
        CHECK_FALSE(store.layered_part("IT-1"));
        CHECK_FALSE(store.layered_part("IT7"));
        CHECK_FALSE(store.layered_part("IT8"));
        // IT2's own row names a missing base: its direct mesh, which IT1's base lookup also kept.
        auto own = store.armor_part_mesh("IT2");
        REQUIRE(own);
        CHECK(own.mesh == store.part_mesh("IT2"));
        CHECK(own.mesh->pieces[0].vertices[0].position.x == 20.0f);
        CHECK_FALSE(own.surface_maps);
        CHECK_FALSE(store.armor_part_mesh("IT9")); // unrelated direct layout cannot fill in a missing layer
        CHECK(store.part_mesh("IT9"));
    }
}

TEST_CASE("hero's forge layers retain omitted surface maps and preserve legacy parts when the layer is unavailable") {
    test::ScratchDir dir("armor_layer_slots");
    armor_fixture_folder(dir.path(), false);
    EqgCharacterStore store{GameDir(dir.path())};
    auto armor = store.armor_part_mesh("IT1");
    REQUIRE(armor);
    CHECK(armor.mesh->materials[0].effect.normal_map == "base_n.dds");
    CHECK(armor.mesh->materials[0].effect.textures == std::vector<std::string>{"base_e.dds"});
    test::ScratchDir incomplete("armor_missing_diffuse");
    armor_fixture_folder(incomplete.path(), true, false);
    EqgCharacterStore broken{GameDir(incomplete.path())};
    auto legacy = broken.armor_part_mesh("IT1");
    REQUIRE(legacy);
    CHECK(legacy.mesh->pieces[0].vertices[0].position.x == 10.0f);
    CHECK_FALSE(legacy.surface_maps);
    CharacterMesh rendered;
    eqg_character::append_pieces(*legacy.mesh, {0}, rendered, legacy.surface_maps);
    REQUIRE_FALSE(rendered.materials.empty());
    CHECK(rendered.materials[0].effect.normal_map.empty());
    CHECK_FALSE(rendered.materials[0].effect.specular);
    CHECK(broken.part_mesh("IT1"));
}

TEST_CASE("hero's forge surface identity keeps native shader and maps without changing legacy material merging") {
    auto source = eqg_character::parse_mesh(armor_fixture_mesh(0.0f), "fixture");
    REQUIRE(source.materials.size() == 2);
    source.materials[0].name = source.materials[1].name = "SHARED";
    source.materials[1].shader = "Chroma_MaxCB1.fx";
    source.materials[1].effect.normal_map = "other_n.dds";
    CharacterMesh generic;
    eqg_character::append_pieces(source, {0}, generic);
    REQUIRE(generic.materials.size() == 1); // legacy identity is name plus diffuse
    CHECK(generic.materials[0].effect.normal_map.empty());
    CharacterMesh armor;
    eqg_character::append_pieces(source, {0}, armor, true);
    REQUIRE(armor.materials.size() == 2);
    CHECK(armor.materials[0].eqg_shader == EqgShader::None);
    CHECK(armor.materials[1].eqg_shader == EqgShader::Chroma);
    CHECK(armor.materials[0].effect.normal_map == "base_n.dds");
    CHECK(armor.materials[1].effect.normal_map == "other_n.dds");
    CHECK(armor.parts[0].material != armor.parts[1].material);
    // Whichever comes first, a plain part never shares an armor part's material (and its maps), nor the reverse.
    for (bool armor_first : {true, false}) {
        CAPTURE(armor_first);
        CharacterMesh mixed;
        eqg_character::append_pieces(source, {0}, mixed, armor_first);
        eqg_character::append_pieces(source, {0}, mixed, !armor_first);
        REQUIRE(mixed.parts.size() == 4);
        REQUIRE(mixed.materials.size() == 3);
        const std::size_t plain = armor_first ? 2 : 0, mapped = armor_first ? 0 : 2;
        for (std::size_t i = plain; i < plain + 2; ++i) {
            CHECK(mixed.materials[mixed.parts[i].material].effect.normal_map.empty());
            CHECK_FALSE(mixed.materials[mixed.parts[i].material].effect.specular);
        }
        CHECK(mixed.parts[plain].material == mixed.parts[plain + 1].material);
        CHECK(mixed.materials[mixed.parts[mapped].material].effect.normal_map == "base_n.dds");
        CHECK(mixed.materials[mixed.parts[mapped + 1].material].effect.normal_map == "other_n.dds");
    }
}

TEST_CASE("EQG character diffuse properties keep exact names and the last matching value") {
    ByteWriter strings;
    auto str = [&](std::string_view value) {
        auto offset = static_cast<std::uint32_t>(strings.size());
        strings.fixed_string(value, value.size() + 1);
        return offset;
    };
    ByteWriter material;
    material.le<std::uint32_t>(0);
    material.le(str("M"));
    material.le(str("Opaque_MaxCB1.fx"));
    material.le<std::uint32_t>(3);
    for (const auto& [key, value] : std::array{
             std::pair{"e_TextureDiffuse0", "FIRST_C.DDS"}, std::pair{"e_TextureDiffuse0", "LAST_C.DDS"},
             std::pair{"E_TEXTUREDIFFUSE0", "WRONG_CASE_C.DDS"}}) {
        material.le(str(key));
        material.le<std::uint32_t>(2);
        material.le(str(value));
    }
    ByteWriter data;
    data.fixed_string("EQGM", 4);
    for (std::uint32_t v : {3u, static_cast<std::uint32_t>(strings.size()), 1u, 0u, 0u, 0u}) data.le(v);
    data.bytes(strings.data());
    data.bytes(material.data());
    auto mesh = eqg_character::parse_mesh(data.data(), "fixture");
    REQUIRE(mesh.materials.size() == 1);
    CHECK(mesh.materials[0].diffuse == "LAST_C.DDS");
}

TEST_CASE("hero's forge map-only layers preserve base diffuse and update only authored shader parameters") {
    test::ScratchDir dir("armor_map_only");
    armor_fixture_folder(dir.path(), true, true, true, false);
    EqgCharacterStore store{GameDir(dir.path())};
    auto armor = store.armor_part_mesh("IT1");
    REQUIRE(armor);
    CHECK(armor.mesh->materials[0].diffuse == "BASE_C.DDS");
    CHECK(armor.mesh->materials[0].effect.normal_map == "surface_n.dds");
    CHECK(armor.mesh->materials[0].effect.shininess == 12.0f);
    test::ScratchDir no_params("armor_absent_properties");
    armor_fixture_folder(no_params.path(), true, true, false);
    EqgCharacterStore bare{GameDir(no_params.path())};
    auto plain = bare.armor_part_mesh("IT1");
    REQUIRE(plain);
    CHECK(plain.mesh->materials[0].diffuse == "surface_c.dds");
    CHECK(plain.mesh->materials[0].effect.normal_map.empty());
    CHECK(plain.mesh->materials[0].effect.textures.empty());
    CHECK(plain.mesh->materials[0].effect.shininess == 40.0f);
    CHECK(plain.mesh->materials[0].effect.env_strength == 0.0f);
}

TEST_CASE("hero's forge aliases bypass colliding WLD armor parts while held items keep the WLD actor") {
    using namespace test::wld_fixtures;
    test::ScratchDir dir("armor_wld_collision");
    armor_fixture_folder(dir.path());
    WldBuilder b;
    auto bitmap = b.add(0x03, b.str("BODY_SPRITE"), bitmap_name("fixture.bmp"));
    ByteWriter info;
    info.le<std::uint32_t>(0);
    info.le<std::uint32_t>(1);
    info.le(bitmap);
    auto sprite_info = b.add(0x04, 0, info);
    ByteWriter ref;
    ref.le(sprite_info);
    ref.le<std::uint32_t>(0);
    auto sprite = b.add(0x05, 0, ref);
    auto mat = b.add(0x30, b.str("BODY_MDF"), material(0x80000001, sprite));
    auto palette = b.add(0x31, 0, refs({mat, mat, mat}));
    auto geometry = b.add(0x36, b.str("HUM_DMSPRITEDEF"), mesh(palette, false));
    ref = {};
    ref.le(geometry);
    ref.le<std::uint32_t>(0);
    auto mesh_ref = b.add(0x2d, 0, ref);
    ByteWriter track;
    track.le<std::int32_t>(0);
    track.le<std::uint32_t>(0);
    const std::array tracks{b.add(0x13, b.str("HUM_ROOT_TRACK"), track),
                            b.add(0x13, b.str("HUMHEAD_POINT_TRACK"), track),
                            b.add(0x13, b.str("HUMTUNIC_POINT_TRACK"), track)};
    ByteWriter skeleton;
    skeleton.le<std::uint32_t>(0);
    skeleton.le<std::uint32_t>(3);
    skeleton.le<std::int32_t>(0);
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        skeleton.le<std::int32_t>(0);
        skeleton.le<std::uint32_t>(0);
        skeleton.le(tracks[i]);
        skeleton.le<std::int32_t>(i == 0 ? mesh_ref : 0);
        skeleton.le<std::uint32_t>(0);
    }
    auto sk = b.add(0x10, b.str("HUM_HS_DEF"), skeleton);
    ref = {};
    ref.le(sk);
    ref.le<std::uint32_t>(0);
    auto sk_ref = b.add(0x11, 0, ref);
    auto actor = [&](const char* name, std::int32_t resource) {
        ByteWriter body;
        for (std::uint32_t v : {0u, 0u, 0u, 1u, 0u}) body.le(v);
        body.le(resource);
        b.add(0x14, b.str(name), body);
    };
    actor("HUM_ACTORDEF", sk_ref);
    actor("IT1_ACTORDEF", mesh_ref);
    actor("IT3_ACTORDEF", mesh_ref); // IT3's row names a missing base
    auto source = std::make_shared<CharacterArchive>();
    source->archive = std::make_shared<PfsArchive>(PfsArchive::from_memory(
        test::pfs_fixtures::make_pfs({{"fixture.wld", b.build()}}), "globalhum_chr.s3d"));
    source->wlds.push_back(wld::WldFile::parse(*source->archive->read("fixture.wld"), "globalhum_chr.s3d:fixture.wld"));
    CharacterLibrary library({source});
    library.set_eqg_store(std::make_shared<EqgCharacterStore>(GameDir(dir.path())));
    auto data = std::make_shared<game::AppearanceData>();
    data->new_armor_filter = game::kNewArmorAllBits;
    data->new_armor.parse_tags("5000\tHUM\t1\t\t-1\t1\t\n5001\tHUM\t3\t\t-1\t1\t\n");
    library.set_appearance_data(data);
    auto model = library.model("HUM");
    REQUIRE(model);
    REQUIRE(model->luclin);
    game::Appearance appearance;
    appearance.show_helm = true;
    appearance.hero_forge[0] = 5000;
    auto dressed = library.dress(*model, appearance);
    REQUIRE(dressed);
    CHECK(std::any_of(dressed->materials.begin(), dressed->materials.end(), [](const Material& m) {
        return m.frames == std::vector<std::string>{"surface_c.dds"};
    }));
    // An unfollowable row (no base) leaves the piece as it was without rows: the WLD actor, not the EQG one.
    appearance.hero_forge[0] = 5001;
    auto unmapped = library.dress(*model, appearance);
    REQUIRE(unmapped);
    CHECK(unmapped->vertices.size() == dressed->vertices.size() - 3 + 4); // EQG piece (3) replaced by the WLD one (4)
    CHECK(std::none_of(unmapped->materials.begin(), unmapped->materials.end(), [](const Material& m) {
        return !m.frames.empty() && (m.frames[0] == "BASE_C.DDS" || m.frames[0] == "base_c.dds");
    }));
    auto held = library.item_mesh(1);
    REQUIRE(held);
    CHECK_FALSE(held->eqg);
    CHECK(held->mesh.vertices.size() == 4);
    CHECK(held->mesh.vertices[0].position.x >= 100.0f);
}

TEST_CASE("modern hero's forge real assets resolve Celestial and Holiday armor separately from held models") {
    // Runs with a live install in EQ_LIVE_DIR (its Resources/Layers rows and modern armor).
    const char* modern_dir = std::getenv("EQ_LIVE_DIR");
    if (!modern_dir) return;
    EqgCharacterStore store{GameDir(modern_dir)};
    auto celestial = store.armor_part_mesh("IT99592");
    REQUIRE(celestial);
    CHECK_FALSE(celestial.mesh->bones.empty());
    CHECK(celestial.mesh->materials[0].diffuse == "robe_celestrial_c.dds");
    CHECK(celestial.mesh->materials[0].effect.normal_map == "robe_celestrial_n.dds");
    auto holiday = store.armor_part_mesh("IT20817");
    auto direct = store.part_mesh("IT20817");
    auto base = store.part_mesh("IT70017");
    REQUIRE(holiday);
    REQUIRE(direct);
    REQUIRE(base);
    CHECK_FALSE(holiday.mesh->bones.empty());
    CHECK(direct->bones.empty());
    CHECK(holiday.mesh->pieces[0].vertices.size() == base->pieces[0].vertices.size());
    CHECK(holiday.mesh->pieces[0].vertices.size() != direct->pieces[0].vertices.size());
    auto old_robe = store.part_mesh("IT28700");
    auto compatible_robe = store.armor_part_mesh("IT28700");
    REQUIRE(old_robe);
    REQUIRE(compatible_robe);
    CHECK(compatible_robe.mesh->pieces[0].vertices.size() == old_robe->pieces[0].vertices.size());
    CHECK(compatible_robe.mesh->materials[0].diffuse == old_robe->materials[0].diffuse);
    CHECK_FALSE(compatible_robe.surface_maps);
    auto library = CharacterLibrary::load(GameDir(modern_dir), "", true);
    auto data = std::make_shared<game::AppearanceData>(*library.appearance_data());
    data->new_armor_filter = game::kNewArmorAllBits;
    library.set_appearance_data(data);
    for (const auto& [code, slot, hero] : std::array{std::tuple{"HIF", 1u, 28507u}, std::tuple{"BAF", 4u, 71004u}}) {
        CAPTURE(code);
        auto model = library.model(code);
        REQUIRE(model);
        game::Appearance appearance;
        appearance.material[slot] = slot == 1 ? 10u : 1u;
        appearance.hero_forge[slot] = hero;
        auto dressed = library.dress(*model, appearance);
        REQUIRE(dressed);
        const std::string& expected = slot == 1 ? celestial.mesh->materials[0].diffuse : holiday.mesh->materials[0].diffuse;
        CHECK(std::any_of(dressed->materials.begin(), dressed->materials.end(), [&](const Material& m) {
            return !m.frames.empty() && m.frames[0] == expected;
        }));
    }
}

TEST_CASE("EQG clips: key times 0 and 0xFFFFFFFF resample to at most kMaxClipFrames frames") {
    eqg_character::MeshFile mesh;
    eqg_character::Bone bone;
    bone.name = "TST_TRACK";
    mesh.bones.push_back(bone);
    mesh.materials.push_back({"M", "Opaque_MaxCB1.fx", "none"});
    eqg_character::Piece piece;
    piece.name = "TST";
    piece.vertices.resize(3);
    piece.triangles.push_back({0, 1, 2});
    piece.triangle_materials.push_back(0);
    mesh.pieces.push_back(piece);

    eqg_character::Track track;
    track.bone = "tst_track";
    track.times_ms = {0, 0xFFFFFFFFu};
    track.poses.resize(2);
    track.poses[1].translation = glm::vec3(0, 0, 1);
    eqg_character::Track normal = track;
    normal.times_ms = {0, 1000};
    std::vector<std::pair<std::string, eqg_character::AnimationFile>> clips = {
        {"L01", eqg_character::AnimationFile{{track}}}, {"P01", eqg_character::AnimationFile{{normal}}}};
    auto m = eqg_character::build_model(mesh, "TST", clips);

    const AnimationClip* bad = m.clip("L01");
    REQUIRE(bad);
    CHECK(bad->frame_ms == 200);
    CHECK(bad->frame_count == eqg_character::kMaxClipFrames);
    CHECK(bad->tracks[0].size() == eqg_character::kMaxClipFrames);
    // A stock-length clip is untouched: 1000 ms at the mean key spacing (clamped to 200 ms) is six frames.
    const AnimationClip* good = m.clip("P01");
    REQUIRE(good);
    CHECK(good->frame_ms == 200);
    CHECK(good->frame_count == 6);
    CHECK(near(good->tracks[0].back().translation, {0, 0, 1}));
}

TEST_CASE("EQG clips: the model's clips together stop at the pose budget; a clip past it is left out") {
    eqg_character::MeshFile mesh;
    for (int i = 0; i < 4; ++i) {
        eqg_character::Bone bone;
        bone.name = "TST_B" + std::to_string(i);
        mesh.bones.push_back(bone);
    }
    mesh.materials.push_back({"M", "Opaque_MaxCB1.fx", "none"});
    eqg_character::Piece piece;
    piece.name = "TST";
    piece.vertices.resize(3);
    piece.triangles.push_back({0, 1, 2});
    piece.triangle_materials.push_back(0);
    mesh.pieces.push_back(piece);
    // Each clip: the four bones at kMaxClipFrames frames (key times 0 and 0xFFFFFFFF).
    eqg_character::AnimationFile file;
    for (int i = 0; i < 4; ++i) {
        eqg_character::Track t;
        t.bone = "tst_b" + std::to_string(i);
        t.times_ms = {0, 0xFFFFFFFFu};
        t.poses.resize(2);
        file.tracks.push_back(t);
    }
    std::vector<std::pair<std::string, eqg_character::AnimationFile>> clips;
    for (const char* name : {"C01", "C02", "C03", "C04", "C05"}) clips.emplace_back(name, file);
    const std::size_t per_clip = 4 * eqg_character::kMaxClipFrames;
    auto m = eqg_character::build_model(mesh, "TST", clips, per_clip * 3);
    CHECK(m.clip("C01"));
    CHECK(m.clip("C03"));
    CHECK_FALSE(m.clip("C04"));
    CHECK_FALSE(m.clip("C05"));
    CHECK(eqg_character::kMaxModelPoses >= per_clip * 100); // stock models stay far below it
}

TEST_CASE("live hero's forge pieces: IT27104 is IT19814 with GENERIC_CHAIN_BLUE_V1's textures") {
    // Runs with a live install in EQ_LIVE_DIR; RoF2's genericchainblue.eqg has IT27104 itself.
    const char* live_dir = std::getenv("EQ_LIVE_DIR");
    if (!live_dir) return;
    EqgCharacterStore store{GameDir(live_dir)};
    auto piece = store.part_mesh("IT27104");
    auto base = store.part_mesh("IT19814");
    REQUIRE(piece);
    REQUIRE(base);
    CHECK(piece->pieces.size() == base->pieces.size());
    REQUIRE(piece->materials.size() == 1);
    CHECK(piece->materials[0].diffuse == "generic_chain_blue_c.dds");
    CHECK(store.has_file("generic_chain_blue_c.dds"));
}

TEST_CASE("EQG textures: c_<code>_ names are read from <code>.eqg over another archive's copies") {
    // Live's and RoF2's mpu.eqg (the puma mount) carry old c_pma_bd_s00_c.dds copies; pma.eqg's are the puma's.
    auto game = hailstone::test::game_dir();
    if (!game) return;
    GameDir dir(*game);
    if (!dir.exists("mpu.eqg") || !dir.exists("pma.eqg")) return;
    EqgCharacterStore store{dir};
    REQUIRE(store.model("MPU")); // opens mpu.eqg first
    auto mount_copy = PfsArchive::open(*dir.find("mpu.eqg")).read("c_pma_bd_s00_c.dds");
    auto own = PfsArchive::open(*dir.find("pma.eqg")).read("c_pma_bd_s00_c.dds");
    REQUIRE(mount_copy);
    REQUIRE(own);
    REQUIRE(*mount_copy != *own);
    CHECK(store.read_file("c_pma_bd_s00_c.dds") == own);
}

TEST_CASE("A model no loaded archive has comes from its own <code>_chr.s3d (live: PBR, the black bear)") {
    // Runs with a live install in EQ_LIVE_DIR: its GlobalLoad.txt loads bea_chr, not RoF2's pbr_chr.
    const char* live_dir = std::getenv("EQ_LIVE_DIR");
    if (!live_dir) return;
    GameDir live(live_dir);
    CharacterLibrary bare(CharacterLibrary::open_archives(live, {"global_chr.s3d"}));
    CHECK_FALSE(bare.has_model("PBR"));
    auto store = std::make_shared<ModelArchiveStore>(live);
    bare.set_model_archive_store(store);
    REQUIRE(bare.has_model("PBR"));
    auto model = bare.model("PBR");
    REQUIRE(model);
    CHECK(model->code == "PBR");
    auto mesh = bare.dress(*model, game::Appearance{});
    REQUIRE(mesh);
    CHECK_FALSE(mesh->materials.empty());
    for (const auto& m : mesh->materials)
        if (!m.frames.empty() && !m.frames.front().starts_with("layers:")) CHECK(bare.has_file(m.frames.front()));
    CHECK_FALSE(store->library("ZZZ")); // no zzz_chr.s3d
}

TEST_CASE("append_wld_mesh: vertex pieces must cover the vertices before their bones are expanded") {
    auto w = wld::WldFile::parse(test::wld_fixtures::mesh_wld(false));
    const wld::Fragment* mesh = nullptr;
    for (const auto& f : w.fragments())
        if (f.holds<wld::MeshDef>()) mesh = &f;
    REQUIRE(mesh);
    wld::Fragment f = *mesh;
    auto& md = f.as<wld::MeshDef>();
    REQUIRE(md.positions.size() == 4);
    md.vertex_pieces = {{{3, 0}}, {{1, 1}}};
    CharacterMesh out;
    detail::append_wld_mesh(w, f, 2, -1, out);
    REQUIRE(out.vertices.size() == 4);
    CHECK(out.vertices[2].bones[0] == 0);
    CHECK(out.vertices[3].bones[0] == 1);
    // Thousands of pieces of 65535 vertices each: refused on the counts alone.
    md.vertex_pieces.assign(4000, {{0xffff, 0}});
    CharacterMesh refused;
    CHECK_THROWS_WITH(detail::append_wld_mesh(w, f, 2, -1, refused), doctest::Contains("do not cover"));
    CHECK(refused.vertices.empty());
    md.vertex_pieces = {{{2, 0}}};
    CHECK_THROWS_WITH(detail::append_wld_mesh(w, f, 2, -1, refused), doctest::Contains("do not cover"));
}

TEST_CASE("GlobalLoad.txt: a phase past int32 wraps as the original's atoi, the same on every platform") {
    test::ScratchDir dir("globalload_phase");
    std::filesystem::create_directories(dir.path() / "Resources");
    std::ofstream(dir.path() / "Resources" / "GlobalLoad.txt", std::ios::binary)
        << "99999999999999999999,0,AAAAC,first_chr.s3d,x\r\n2,0,AAAAC,second_chr.s3d,x\r\n";
    std::ofstream(dir.path() / "first_chr.s3d") << "x";
    std::ofstream(dir.path() / "second_chr.s3d") << "x";
    auto out = global_character_archives(GameDir(dir.path()));
    // 99999999999999999999 wraps to 1661992959, so phase 2 loads first (glibc's atoi made it -1).
    REQUIRE(out.size() >= 2);
    CHECK(out[0] == "second_chr.s3d");
    CHECK(out[1] == "first_chr.s3d");
}

TEST_CASE("long_track_archives: GlobalLoad.txt's fourth FLAGS letter and the Luclin player models") {
    test::ScratchDir dir("long_track_archives");
    std::filesystem::create_directories(dir.path() / "Resources");
    std::ofstream(dir.path() / "Resources" / "GlobalLoad.txt", std::ios::binary)
        << "1,1,TFFTC,GlobalPCFroglok_chr,x\r\n1,1,TFFFC,GlobalDRK_chr,x\r\n3,1,TFFFC,Global_chr,x\r\n"
           "1,1,TFFtC,lower_chr.s3d,x\r\n1,1,TFT,short_chr,x\r\n";
    for (const char* f : {"globaldaf_chr.s3d", "globaldaf_chr2.s3d", "lgequip.s3d"}) std::ofstream(dir.path() / f) << "x";
    const GameDir game(dir.path());
    auto classic = long_track_archives(game);
    CHECK(classic.contains("globalpcfroglok_chr.s3d"));
    CHECK(classic.contains("lower_chr.s3d"));
    CHECK_FALSE(classic.contains("globaldrk_chr.s3d"));
    CHECK_FALSE(classic.contains("global_chr.s3d"));
    CHECK_FALSE(classic.contains("short_chr.s3d"));
    CHECK_FALSE(classic.contains("globaldaf_chr.s3d"));
    auto luclin = long_track_archives(game, true);
    CHECK(luclin.contains("globaldaf_chr.s3d"));
    CHECK(luclin.contains("globaldaf_chr2.s3d"));
    CHECK_FALSE(luclin.contains("lgequip.s3d"));
    CHECK_FALSE(luclin.contains("global_chr.s3d"));
}

TEST_CASE("CharacterMesh serial: every mesh has its own, a copy a new one, a move keeps it") {
    CharacterMesh a, b;
    CHECK(a.serial.value() != 0);
    CHECK(a.serial.value() != b.serial.value());
    const std::uint64_t was = a.serial.value();
    CharacterMesh copy = a;
    CHECK(copy.serial.value() != was);
    CHECK(a.serial.value() == was);
    CharacterMesh moved = std::move(a);
    CHECK(moved.serial.value() == was);
    CHECK(a.serial.value() != was); // the source, reused, is another mesh
    b = moved;
    CHECK(b.serial.value() != was);
    // A mesh freed and another made in its place (the same address, as a re-dressed spawn's can be) differ.
    auto first = std::make_unique<CharacterMesh>();
    const std::uint64_t first_serial = first->serial.value();
    first.reset();
    auto second = std::make_unique<CharacterMesh>();
    CHECK(second->serial.value() != first_serial);
}

namespace {
// An uncompressed 32-bit TGA of one colour.
std::vector<std::byte> solid_tga(std::uint16_t w, std::uint16_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                 std::uint8_t a) {
    ByteWriter out;
    out.le<std::uint8_t>(0);
    out.le<std::uint8_t>(0);
    out.le<std::uint8_t>(2);
    for (int i = 0; i < 9; ++i) out.le<std::uint8_t>(0); // colour map, origin
    out.le(w);
    out.le(h);
    out.le<std::uint8_t>(32);
    out.le<std::uint8_t>(8);
    for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
        out.le(b);
        out.le(g);
        out.le(r);
        out.le(a);
    }
    return out.take();
}
} // namespace

TEST_CASE("Luclin layers: composited at the size of the layer with the most texels") {
    using namespace hailstone::test::pfs_fixtures;
    // A wide skin and a tall armour layer: once composited at the widest by the tallest (a forged pair of 16384 x 1
    // and 1 x 16384 layers made a 1 GiB image).
    auto archive = std::make_shared<CharacterArchive>();
    archive->archive = std::make_shared<const PfsArchive>(PfsArchive::from_memory(
        make_pfs({{"skin.tga", solid_tga(64, 2, 200, 0, 0, 255)}, {"armour.tga", solid_tga(2, 16, 0, 0, 200, 255)},
                  {"big.tga", solid_tga(32, 32, 0, 200, 0, 255)}})));
    CharacterLibrary lib({archive});
    std::string why;
    auto two = lib.texture("layers:skin.tga+armour.tga", false, &why);
    REQUIRE_MESSAGE(two, why);
    CHECK(two->width == 64); // 128 texels against 32
    CHECK(two->height == 2);
    REQUIRE(two->mips.size() == 1);
    REQUIRE(two->mips[0].size() == 64u * 2 * 4);
    CHECK(std::to_integer<int>(two->mips[0][2]) == 200); // the opaque armour layer over the skin
    auto three = lib.texture("layers:skin.tga+big.tga", false, &why);
    REQUIRE(three);
    CHECK(three->width == 32); // the base layer is resampled up to the larger one
    CHECK(three->height == 32);
    CHECK(std::to_integer<int>(three->mips[0][1]) == 200);
}

TEST_CASE("EQGA: a bone name past 256 bytes is refused, not copied for every track") {
    auto clip = [](std::size_t name_length) {
        ByteWriter w;
        w.fixed_string("EQGA", 4);
        w.le<std::uint32_t>(1);
        w.le<std::uint32_t>(static_cast<std::uint32_t>(name_length + 1));
        w.le<std::uint32_t>(1); // tracks
        w.fixed_string(std::string(name_length, 'b'), name_length + 1);
        w.le<std::uint32_t>(0); // frames
        w.le<std::uint32_t>(0); // the name at offset 0
        return w.take();
    };
    CHECK(eqg_character::parse_animation(clip(256)).tracks.at(0).bone.size() == 256);
    CHECK_THROWS_WITH(eqg_character::parse_animation(clip(257)), doctest::Contains("too long"));
}

TEST_CASE("RoF2 cleric epic held models keep their authored pearl environment surface" *
          doctest::skip(!hailstone::test::game_dir())) {
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "");
    auto item = lib.item_mesh(10738);
    REQUIRE(item);
    CHECK(item->eqg);
    CHECK(item->emitters.size() == 3);
    CHECK(std::any_of(item->mesh.materials.begin(), item->mesh.materials.end(), [](const Material& m) {
        return m.effect.env_strength > 0.6f && !m.effect.normal_map.empty() && !m.effect.textures.empty();
    }));
}

TEST_CASE("RoF2 held models retain the native packed environment material aliases" *
          doctest::skip(!hailstone::test::game_dir())) {
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "");
    // CBGGE1 (enchanter/paladin epics), CBSE1 and CBE1 all select the native CBSGE1 render class.
    for (std::uint32_t number : {10740u, 10744u, 10776u, 11453u}) {
        CAPTURE(number);
        auto item = lib.item_mesh(number);
        REQUIRE(item);
        CHECK(item->eqg);
        CHECK(std::any_of(item->mesh.materials.begin(), item->mesh.materials.end(), [](const Material& m) {
            return m.effect.packed_normal && m.effect.glow && m.effect.reflective &&
                   !m.effect.normal_map.empty() && !m.effect.textures.empty() && m.effect.env_strength > 0.0f;
        }));
    }
}

TEST_CASE("RoF2 evil-tree held items retain packed glow surfaces and valid always-on sparkle attachments" *
          doctest::skip(!hailstone::test::game_dir())) {
    GameDir dir(*hailstone::test::game_dir());
    if (!dir.exists("rofequip.eqg")) return;
    auto lib = CharacterLibrary::load(dir, "");
    auto edd = dir.read("actoremittersnew.edd");
    REQUIRE(edd);
    const auto definitions = parse_edd(*edd);
    struct Item { std::uint32_t number, emitter; glm::vec3 point; };
    for (const Item item : std::array{Item{12459, 846, {2.886f, 0, 0}}, Item{12461, 849, {0, 0.439f, 0}}}) {
        CAPTURE(item.number);
        auto held = lib.item_mesh(item.number);
        REQUIRE(held);
        CHECK(held->eqg);
        REQUIRE(held->mesh.materials.size() == 2);
        const auto& surface = held->mesh.materials[0];
        CHECK(surface.blend == BlendMode::Opaque);
        CHECK(surface.effect.packed_normal);
        CHECK(surface.effect.glow);
        CHECK(surface.effect.specular);
        CHECK(surface.effect.shininess == doctest::Approx(128.0f));
        CHECK(surface.effect.normal_map == "eviltree_weapons01_n.dds");
        CHECK_FALSE(surface.effect.reflective);
        CHECK(surface.effect.textures.empty()); // this pair has authored glow/gloss, no environment-map property
        const auto& cutout = held->mesh.materials[1];
        CHECK(cutout.blend == BlendMode::Masked);
        CHECK(cutout.effect.normal_map == surface.effect.normal_map);
        CHECK_FALSE(cutout.effect.glow);
        CHECK_FALSE(cutout.effect.specular);
        REQUIRE(surface.frames.size() == 1);
        CHECK(surface.frames[0] == "eviltree_weapons01_c.dds");
        auto normal = lib.texture(surface.effect.normal_map, false);
        REQUIRE(normal);
        CHECK(normal->width == 256);
        CHECK(normal->height == 256);
        REQUIRE(held->emitters.size() == 1);
        const auto& emitter = held->emitters.front();
        CHECK(emitter.emitter == item.emitter);
        CHECK(emitter.trigger == 0);
        CHECK(emitter.variant == -1);
        const auto* point = find_point(held->points, emitter.point);
        REQUIRE(point);
        CHECK(point->bone == "ATTACH_TO_ORIGIN");
        CHECK(glm::length(point->offset - item.point) < 0.001f);
        REQUIRE(emitter.emitter < definitions.size());
        const auto& definition = definitions[emitter.emitter];
        CHECK(definition.additive == 1);
        CHECK_FALSE(definition.texture.empty());
        CHECK((dir.exists("SpellEffects/" + definition.texture) || dir.exists("ActorEffects/" + definition.texture) ||
               dir.exists("EnvEmitterEffects/" + definition.texture)));
    }
}

TEST_CASE("RoF2 MPL held items retain their authored coverage texture and UV scale" *
          doctest::skip(!hailstone::test::game_dir())) {
    GameDir dir(*hailstone::test::game_dir());
    if (!dir.exists("hotequip.eqg")) return;
    auto lib = CharacterLibrary::load(dir, "");
    auto held = lib.item_mesh(11707);
    REQUIRE(held);
    // The source also contains unused "01" materials; this is the authored surface referenced by its triangles.
    const auto material = std::find_if(held->mesh.materials.begin(), held->mesh.materials.end(), [](const Material& m) {
        return m.name == "nightmare02";
    });
    REQUIRE(material != held->mesh.materials.end());
    CHECK(material->effect.coverage_map == "grey_coverage.dds");
    CHECK(material->effect.coverage_scale == doctest::Approx(0.05f));
    CHECK(material->effect.normal_map == "nightmare_magic_weapons_normal02.dds");
    CHECK_FALSE(material->effect.packed_normal);
    CHECK_FALSE(material->effect.glow);
    CHECK(lib.texture(material->effect.coverage_map, false));
}
