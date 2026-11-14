#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/character.h"
#include "assets/eqg_character.h"
#include "assets/pfs.h"
#include "core/game_dir.h"
#include "game/appearance.h"
#include "game/race_models.h"
#include "game_files.h"
#include "scratch_dir.h"
#include "pfs_fixtures.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;

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
        // BOAT is a static actor and IVM has no visible mesh in the shipped files.
        if (code == "BOAT" || code == "IVM") continue;
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
