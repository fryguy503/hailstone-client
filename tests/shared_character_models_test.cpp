#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "assets/character.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "core/shared_file_cache.h"
#include "game_files.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {
void same_component(float got, float want) {
    if (std::isnan(want)) {
        CHECK(std::isnan(got));
    } else if (std::isinf(want)) {
        CHECK(got == want);
    } else {
        CHECK(std::isfinite(got));
        CHECK(got == doctest::Approx(want).epsilon(1e-6).scale(1.0));
    }
}

void same_sample(const CharacterModel& mapped, const AnimationClip& mapped_clip,
                 const CharacterModel& owned, const AnimationClip& owned_clip, float time, bool loop) {
    CAPTURE(time);
    CAPTURE(loop);
    std::vector<BonePose> got, want;
    mapped.sample_local(mapped_clip, time, loop, got);
    owned.sample_local(owned_clip, time, loop, want);
    REQUIRE(got.size() == want.size());
    for (std::size_t bone = 0; bone < want.size(); ++bone) {
        for (int component = 0; component < 4; ++component)
            same_component(got[bone].rotation[component], want[bone].rotation[component]);
        for (int component = 0; component < 3; ++component) {
            same_component(got[bone].translation[component], want[bone].translation[component]);
            same_component(got[bone].scale[component], want[bone].scale[component]);
        }
    }
}

struct RetainedModels {
    std::shared_ptr<const CharacterModel> owned;
    std::shared_ptr<const CharacterModel> mapped;
    std::string mapped_clip;
};

std::shared_ptr<const CharacterArchive> track_archive(const std::string& label, const std::string& name,
                                                     std::int32_t reference) {
    test::wld_fixtures::WldBuilder builder;
    ByteWriter body;
    body.le(reference);
    body.le<std::uint32_t>(0);
    builder.add(0x13, builder.str(name), body);
    auto archive = std::make_shared<CharacterArchive>();
    archive->archive = std::make_shared<const PfsArchive>(PfsArchive::from_memory(
        test::pfs_fixtures::make_pfs({{"fixture.wld", builder.build()}}), label));
    archive->wlds.push_back(wld::WldFile::parse(*archive->archive->read("fixture.wld")));
    return archive;
}

std::shared_ptr<const CharacterArchive> assignment_archive(const std::string& label, float translation) {
    using namespace test::wld_fixtures;
    WldBuilder builder;
    const auto materials = builder.add(0x31, 0, refs({0, 0, 0}));
    const auto geometry = builder.add(0x36, builder.str("TST_DMSPRITEDEF"), mesh(materials, false));
    ByteWriter body;
    body.le(geometry);
    body.le<std::uint32_t>(0);
    const auto mesh_ref = builder.add(0x2d, 0, body);
    const std::string root_name = "TST_LONG_HEAP_OWNED_ASSIGNMENT_ROOT_TRACK";
    auto add_track = [&](const std::string& name) {
        ByteWriter keys;
        keys.le<std::uint32_t>(0);
        keys.le<std::uint32_t>(1);
        for (float value : {1.0f, translation, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f}) keys.f32(value);
        const auto definition = builder.add(0x12, 0, keys);
        ByteWriter reference;
        reference.le(definition);
        reference.le<std::uint32_t>(0);
        return builder.add(0x13, builder.str(name), reference);
    };
    const auto rest = add_track(root_name);
    add_track("L01" + root_name);
    body = {};
    for (std::uint32_t value : {0u, 1u, 0u}) body.le(value);
    body.le(builder.str("TST_ROOT_DAG"));
    body.le<std::uint32_t>(0);
    body.le(rest);
    body.le(mesh_ref);
    body.le<std::uint32_t>(0);
    const auto skeleton = builder.add(0x10, builder.str("TST_HS_DEF"), body);
    body = {};
    body.le(skeleton);
    body.le<std::uint32_t>(0);
    const auto skeleton_ref = builder.add(0x11, 0, body);
    body = {};
    for (std::uint32_t value : {0u, 0u, 0u, 1u, 0u}) body.le(value);
    body.le(skeleton_ref);
    builder.add(0x14, builder.str("TST_ACTORDEF"), body);
    auto archive = std::make_shared<CharacterArchive>();
    archive->archive = std::make_shared<const PfsArchive>(PfsArchive::from_memory(
        test::pfs_fixtures::make_pfs({{"fixture.wld", builder.build()}}), label));
    archive->wlds.push_back(wld::WldFile::parse(*archive->archive->read("fixture.wld")));
    return archive;
}
} // namespace

TEST_CASE("shared character archive track names retain level resolution and own recorded lookup names") {
    const std::string name = "L01LONG_TRACK_NAME_WITH_A_HEAP_OWNED_FRAGMENT_STRING_TRACK";
    const auto global = track_archive("global_chr.s3d", name, 10);
    const auto zone = track_archive("zone_chr.s3d", name, 20);
    const auto later = track_archive("later_chr.s3d", name, 30);
    detail::CharacterTables base, overlay;
    base.add(*global, 0, 0, nullptr, nullptr);
    REQUIRE(base.tracks.size() == 1);
    CHECK(base.tracks.begin()->first.data() == global->wlds[0].fragments()[0].name.data());
    CHECK(base.track_levels.begin()->first.data() == global->wlds[0].fragments()[0].name.data());
    std::unordered_set<std::string> defined;
    overlay.add(*zone, 1, 0, &base, &defined);
    CHECK(overlay.tracks.empty()); // the first definition at the same level still wins
    overlay.add(*zone, 1, 2, &base, &defined);
    REQUIRE(overlay.tracks.size() == 1);
    CHECK(overlay.tracks.at(name).ref.track == 20);
    CHECK(overlay.track_levels.at(name) == ((std::uint64_t{1} << 0) | (std::uint64_t{1} << 2)));
    overlay.add(*later, 2, 2, &base, &defined);
    CHECK(overlay.tracks.at(name).ref.track == 20);
    overlay.add(*later, 2, 3, &base, &defined);
    CHECK(overlay.tracks.at(name).ref.track == 30);
    CHECK(base.tracks.at(name).ref.track == 10);
    CHECK(defined.contains("t" + name));
    // Copies keep views into the same archives, which their enclosing libraries retain.
    auto copied = overlay;
    overlay.tracks.at(name).ref.track = 40;
    CHECK(copied.tracks.at(name).ref.track == 30);
    overlay = {};
    detail::TableView<'t', decltype(copied.tracks)> view(&copied.tracks, &base.tracks);
    REQUIRE(view.find(name));
    CHECK(view.find(name)->ref.track == 30);
    CharacterLookups::Names recorded;
    {
        CharacterLookups lookups;
        std::string temporary = name;
        CHECK(view.contains(temporary));
        temporary.clear();
        recorded = lookups.names();
    }
    REQUIRE(recorded);
    CHECK(std::find(recorded->begin(), recorded->end(), "t" + name) != recorded->end());
}

TEST_CASE("shared character archive library assignment replaces borrowed keys while retaining their owners") {
    CharacterLibrary target({assignment_archive("old.s3d", 11.0f)});
    std::weak_ptr<const CharacterArchive> replaced = target.archives().front();
    {
        const CharacterLibrary source({assignment_archive("copied.s3d", 22.0f)});
        target = source;
        CHECK(replaced.expired());
    }
    auto check = [](const CharacterLibrary& library, const std::string& label, float translation) {
        REQUIRE(library.archives().size() == 1);
        CHECK(library.archives()[0]->archive->label() == label);
        const auto model = library.build("TST");
        REQUIRE(model.clips.contains("L01"));
        CHECK(model.clips.at("L01").tracks[0].front().translation.x == translation);
    };
    check(target, "copied.s3d", 22);
    auto* alias = &target;
    target = *alias;
    check(target, "copied.s3d", 22);
    target = std::move(*alias);
    check(target, "copied.s3d", 22);
    replaced = target.archives().front();
    {
        CharacterLibrary source({assignment_archive("moved.s3d", 33.0f)});
        target = std::move(source);
        CHECK(replaced.expired());
    }
    check(target, "moved.s3d", 33);
}

TEST_CASE("shared character archive index assignment keeps replacement table keys backed through copy move and self assignment") {
    CharacterArchiveIndex target({assignment_archive("old.s3d", 11.0f)});
    std::weak_ptr<const CharacterArchive> replaced = target.archives().front();
    {
        const CharacterArchiveIndex source({assignment_archive("copied.s3d", 22.0f)});
        target = source;
        CHECK(replaced.expired());
    }
    auto check = [](const CharacterArchiveIndex& index, const std::string& label, float translation) {
        REQUIRE(index.archives().size() == 1);
        CHECK(index.archives()[0]->archive->label() == label);
        const CharacterLibrary library(std::make_shared<const CharacterArchiveIndex>(index), {});
        const auto model = library.build("TST");
        REQUIRE(model.clips.contains("L01"));
        CHECK(model.clips.at("L01").tracks[0].front().translation.x == translation);
    };
    check(target, "copied.s3d", 22);
    auto* alias = &target;
    target = *alias;
    check(target, "copied.s3d", 22);
    target = std::move(*alias);
    check(target, "copied.s3d", 22);
    replaced = target.archives().front();
    {
        CharacterArchiveIndex source({assignment_archive("moved.s3d", 33.0f)});
        target = std::move(source);
        CHECK(replaced.expired());
    }
    check(target, "moved.s3d", 33);
}

TEST_CASE("shared character archive models match private stock clips and retain mapped keys after library destruction"
          * doctest::skip(!test::game_dir())) {
    test::ScratchDir scratch("shared_character_models");
    const auto root = std::filesystem::canonical(scratch.path());
    const auto cache_path = root / "cache";
    std::filesystem::create_directory(root / "private-overlay");
    std::filesystem::create_directory(root / "mapped-overlay");
    std::vector<RetainedModels> retained;
    std::size_t mapped_tracks = 0;
    {
        GameDir private_game(test::game_dir()->root(), root / "private-overlay");
        GameDir mapped_game(test::game_dir()->root(), root / "mapped-overlay");
        auto shared_cache = std::make_shared<SharedFileCache>(SharedFileCache::Options{
            cache_path, std::uint64_t{4} << 30});
        mapped_game.set_shared_file_cache(shared_cache);
        CharacterArchiveCache private_archives(true), mapped_archives(true);
        auto private_library = private_archives.library_for_zone(private_game, "gfaydark");
        auto mapped_library = mapped_archives.library_for_zone(mapped_game, "gfaydark");
        REQUIRE(private_library);
        REQUIRE(mapped_library);
        CharacterLibrary private_copy = *private_library;
        CharacterLibrary mapped_copy = *mapped_library;
        private_library.reset();
        mapped_library.reset();
        private_archives.clear();
        mapped_archives.clear();
        for (const auto* code : {"HUM", "HUF", "ERM", "ELM"}) {
            CAPTURE(code);
            const bool present = private_copy.has_model(code);
            CHECK(mapped_copy.has_model(code) == present);
            if (!present) continue;
            auto owned = private_copy.model(code);
            auto mapped = mapped_copy.model(code);
            REQUIRE(owned);
            REQUIRE(mapped);
            CHECK(mapped->animation_donor == owned->animation_donor);
            CHECK(mapped->bone_names == owned->bone_names);
            CHECK(mapped->parents == owned->parents);
            CHECK(mapped->order == owned->order);
            CHECK(mapped->clip_aliases == owned->clip_aliases);
            CHECK(mapped->held_aliases == owned->held_aliases);
            REQUIRE(mapped->clips.size() == owned->clips.size());
            std::string mapped_clip;
            for (const auto& [name, expected] : owned->clips) {
                CAPTURE(name);
                const auto found = mapped->clips.find(name);
                REQUIRE(found != mapped->clips.end());
                const auto& actual = found->second;
                CHECK(actual.frame_count == expected.frame_count);
                CHECK(actual.frame_ms == expected.frame_ms);
                CHECK(actual.borrowed == expected.borrowed);
                REQUIRE(actual.tracks.size() == expected.tracks.size());
                for (std::size_t bone = 0; bone < expected.tracks.size(); ++bone) {
                    CHECK(actual.tracks[bone].size() == expected.tracks[bone].size());
                    if (actual.tracks[bone].mapped()) {
                        ++mapped_tracks;
                        mapped_clip = name;
                        CHECK(actual.tracks[bone].decoded_capacity() == 0);
                    }
                }
                const float key_seconds = static_cast<float>(expected.frame_ms) / 1000.0f;
                const float beyond_end = key_seconds * (static_cast<float>(expected.frame_count) + 0.5f);
                for (bool loop : {false, true})
                    for (float time : std::array{0.0f, key_seconds * 0.5f, key_seconds, beyond_end})
                        same_sample(*mapped, actual, *owned, expected, time, loop);
                for (const auto& track : actual.tracks)
                    if (track.mapped()) CHECK(track.decoded_capacity() == 0);
            }
            retained.push_back({std::move(owned), std::move(mapped), std::move(mapped_clip)});
        }
        CHECK(shared_cache->stats().mapped_bytes > 0);
    }
    REQUIRE_FALSE(retained.empty());
    REQUIRE(mapped_tracks > 0);
    SharedFileCache cleanup({cache_path, std::uint64_t{4} << 30});
    CHECK_FALSE(cleanup.trim());
    std::size_t survivors = 0;
    for (const auto& models : retained) {
        if (models.mapped_clip.empty()) continue;
        const auto& mapped_clip = models.mapped->clips.at(models.mapped_clip);
        const auto& owned_clip = models.owned->clips.at(models.mapped_clip);
        for (bool loop : {false, true})
            same_sample(*models.mapped, mapped_clip, *models.owned, owned_clip, 0.35f, loop);
        for (const auto& track : mapped_clip.tracks) {
            if (!track.mapped()) continue;
            ++survivors;
            CHECK(track.decoded_capacity() == 0);
        }
    }
    CHECK(survivors > 0);
    retained.clear();
    CHECK(cleanup.trim());
}
