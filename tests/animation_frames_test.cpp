#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "assets/character.h"
#include "core/byte_io.h"
#include "core/shared_file_cache.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;

static_assert(std::random_access_iterator<BoneFrames::const_iterator>);
// Values are decoded on dereference, so the C++17 category is only input.
static_assert(std::is_same_v<std::iterator_traits<BoneFrames::const_iterator>::iterator_category, std::input_iterator_tag>);

namespace {
std::vector<wld::TrackFrame> unpacked_frames() {
    std::vector<wld::TrackFrame> frames(6);
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::bit_cast<float>(0x7fc12345u);
    const std::array<glm::quat, 6> rotations{{{2, -1, 3, -4}, {0, 0, 0, 0}, {1e-8f, 0, 0, 0},
                                           {infinity, 1, 0, 0}, {nan, 0, 1, 0}, {-2, 0, 0, 0}}};
    const std::array<float, 6> scales{2.0f, 1.0f, -0.0f, infinity, nan, -1.0f};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i].rotation = rotations[i];
        frames[i].translation = {static_cast<float>(i + 1), -0.0f, -2.25f};
        frames[i].scale = scales[i];
    }
    return frames;
}

std::vector<std::byte> unpacked_bytes(const std::vector<wld::TrackFrame>& frames) {
    ByteWriter out;
    for (const auto& frame : frames) {
        out.f32(frame.scale);
        for (int i = 0; i < 3; ++i) out.f32(frame.translation[i]);
        for (const auto value : {frame.rotation.w, frame.rotation.x, frame.rotation.y, frame.rotation.z}) out.f32(value);
    }
    return out.take();
}

BonePose reference_pose(const wld::TrackFrame& frame, glm::vec3 multiplier = glm::vec3(1.0f)) {
    BonePose out;
    const auto q = frame.rotation;
    const auto length = glm::length(q);
    out.rotation = std::isfinite(length) && length > 1e-6f ? q / length : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    out.translation = frame.translation;
    out.scale = glm::vec3(std::isfinite(frame.scale) ? frame.scale : 1.0f);
    out.scale *= multiplier;
    return out;
}

void same_pose(const BonePose& a, const BonePose& b) {
    for (int i = 0; i < 4; ++i)
        CHECK(std::bit_cast<std::uint32_t>(a.rotation[i]) == std::bit_cast<std::uint32_t>(b.rotation[i]));
    for (int i = 0; i < 3; ++i) {
        CHECK(std::bit_cast<std::uint32_t>(a.translation[i]) == std::bit_cast<std::uint32_t>(b.translation[i]));
        CHECK(std::bit_cast<std::uint32_t>(a.scale[i]) == std::bit_cast<std::uint32_t>(b.scale[i]));
    }
}

std::vector<std::byte> animation_model_wld() {
    using namespace test::wld_fixtures;
    WldBuilder builder;
    const auto bitmap = builder.add(0x03, builder.str("FIXTURE_SPRITE"), bitmap_name("fixture.bmp"));
    ByteWriter body;
    body.le<std::uint32_t>(0);
    body.le<std::uint32_t>(1);
    body.le(bitmap);
    const auto info = builder.add(0x04, 0, body);
    body = {};
    body.le(info);
    body.le<std::uint32_t>(0);
    const auto sprite = builder.add(0x05, 0, body);
    const auto mat = builder.add(0x30, builder.str("FIXTURE_MDF"), material(0x80000001, sprite));
    const auto materials = builder.add(0x31, 0, refs({mat, mat, mat}));
    const auto geometry = builder.add(0x36, builder.str("ERM_DMSPRITEDEF"), mesh(materials, false));
    body = {};
    body.le(geometry);
    body.le<std::uint32_t>(0);
    const auto mesh_ref = builder.add(0x2d, 0, body);
    auto add_track = [&](const std::string& name, const std::vector<wld::TrackFrame>& frames) {
        ByteWriter keys;
        keys.le<std::uint32_t>(0);
        keys.le(static_cast<std::uint32_t>(frames.size()));
        keys.bytes(unpacked_bytes(frames));
        const auto definition = builder.add(0x12, 0, keys);
        ByteWriter reference;
        reference.le(definition);
        reference.le<std::uint32_t>(1);
        reference.le<std::uint32_t>(100);
        return builder.add(0x13, builder.str(name), reference);
    };
    wld::TrackFrame rest, hidden;
    hidden.scale = 0.02f;
    const std::array rest_refs{add_track("ERM_TRACK", {rest}), add_track("ERMTUNIC_POINT_TRACK", {hidden})};
    auto moving = rest;
    moving.rotation = glm::quat(-2, 1, 0, 0);
    moving.translation = {1, 2, 3};
    for (const auto* suffix : {"_TRACK", "TUNIC_POINT_TRACK"}) {
        add_track(std::string("C01AERM") + suffix, {rest, moving});
        add_track(std::string("D01HUM") + suffix, {rest, moving});
    }
    ByteWriter skeleton;
    skeleton.le<std::uint32_t>(0);
    skeleton.le<std::uint32_t>(2);
    skeleton.le<std::int32_t>(0);
    for (std::size_t i = 0; i < rest_refs.size(); ++i) {
        skeleton.le(builder.str(i == 0 ? "ERM_ROOT_DAG" : "ERMTUNIC_POINT_DAG"));
        skeleton.le<std::uint32_t>(0);
        skeleton.le(rest_refs[i]);
        skeleton.le<std::int32_t>(i == 0 ? mesh_ref : 0);
        skeleton.le<std::uint32_t>(i == 0 ? 1 : 0);
        if (i == 0) skeleton.le<std::int32_t>(1);
    }
    const auto definition = builder.add(0x10, builder.str("ERM_HS_DEF"), skeleton);
    body = {};
    body.le(definition);
    body.le<std::uint32_t>(0);
    const auto reference = builder.add(0x11, 0, body);
    body = {};
    for (std::uint32_t value : {0u, 0u, 0u, 1u, 0u}) body.le(value);
    body.le(reference);
    builder.add(0x14, builder.str("ERM_ACTORDEF"), body);
    return builder.build();
}
} // namespace

TEST_CASE("shared animation frames: unpacked views preserve pose normalization sign and nonfinite rules") {
    const auto source = unpacked_frames();
    const auto frames = wld::TrackFrames::from_encoded(ImmutableFileBytes::owned(unpacked_bytes(source)), 0, source.size(), false);
    const auto keys = BoneFrames::from_wld(frames);
    CHECK(keys.encoded());
    CHECK_FALSE(keys.mapped());
    CHECK(keys.decoded_capacity() == 0);
    REQUIRE(keys.size() == source.size());
    for (std::size_t i = 0; i < keys.size(); ++i) same_pose(keys[i], reference_pose(source[i]));
    CHECK(keys[0].rotation.w > 0.0f);
    CHECK(keys[0].rotation.x < 0.0f);
    for (std::size_t i : {1u, 2u, 3u, 4u}) CHECK(keys[i].rotation.w == 1.0f);
    CHECK(keys[5].rotation.w == -1.0f); // preserve the source quaternion's sign
    CHECK(std::signbit(keys[2].scale.x));
    CHECK(keys[3].scale.x == 1.0f);
    CHECK(keys[4].scale.x == 1.0f);
    const std::vector<BonePose> iterated(keys.begin(), keys.end());
    REQUIRE(iterated.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) same_pose(iterated[i], keys[i]);
    same_pose(keys.front(), keys[0]);
    same_pose(keys.back(), keys.at(keys.size() - 1));
    same_pose(keys.begin()[2], keys[2]);
    same_pose(*(1 + keys.begin()), keys[1]);
    CHECK(keys.end() - keys.begin() == static_cast<std::ptrdiff_t>(keys.size()));
    CHECK_THROWS_AS(keys.at(keys.size()), std::out_of_range);
    CHECK(keys.decoded_capacity() == 0);
}

TEST_CASE("shared animation frames: packed mapped aliases retain backing and borrowed scale without private poses") {
    test::ScratchDir dir("animation-mapped-keys");
    const auto directory = std::filesystem::canonical(dir.path()) / "cache";
    auto cache = std::make_unique<SharedFileCache>(SharedFileCache::Options{directory});
    ByteWriter raw;
    for (const auto value : std::array<std::int16_t, 7>{-32768, 8192, -4096, 2048, -32768, 32767, -1234}) raw.le(value);
    raw.le<std::uint16_t>(65535);
    auto owner = cache->store(raw.take());
    REQUIRE(owner.mapped());
    auto source = wld::TrackFrames::from_encoded(std::move(owner), 0, 1, true);
    const auto expected = reference_pose(std::as_const(source).front(), {0.02f, 0.03f, 0.04f});
    AnimationClip clip;
    clip.tracks.push_back(BoneFrames::from_wld(source, {0.02f, 0.03f, 0.04f}));
    auto alias = clip;
    source.clear();
    clip = {};
    cache.reset();
    SharedFileCache cleanup({directory});
    (void)cleanup.trim(0); // removes at most the name (POSIX); the keys keep their mapping
    const auto& keys = std::as_const(alias).tracks[0];
    REQUIRE(keys.mapped());
    same_pose(keys.front(), expected);
    CHECK(keys.decoded_capacity() == 0);
    auto moved = std::move(alias.tracks[0]);
    CHECK(alias.tracks[0].empty());
    same_pose(std::as_const(moved).front(), expected);
    moved.clear();
    CHECK(cleanup.trim(0));
}

TEST_CASE("shared animation frames: fixture mutations detach one view and EQG owned poses remain direct") {
    const auto source = unpacked_frames();
    const auto original = BoneFrames::from_wld(wld::TrackFrames::from_encoded(
        ImmutableFileBytes::owned(unpacked_bytes(source)), 0, source.size(), false));
    auto changed = original;
    CHECK_THROWS_AS(changed.at(changed.size()), std::out_of_range);
    CHECK(changed.encoded());
    CHECK(changed.decoded_capacity() == 0);
    changed[0].translation.x = 42.0f;
    CHECK_FALSE(changed.encoded());
    CHECK(changed.decoded_capacity() >= source.size());
    CHECK(std::as_const(changed).front().translation.x == 42.0f);
    CHECK(original.front().translation.x == 1.0f);
    changed.begin()->scale.y = 3.0f;
    changed.push_back(original.front());
    changed.emplace_back();
    changed.resize(10, original.front());
    changed.reserve(12);
    CHECK(changed.capacity() >= 12);
    changed.data()[9].scale.z = 7.0f;
    CHECK(std::as_const(changed).back().scale.z == 7.0f);
    changed.assign({original.back()});
    same_pose(std::as_const(changed).front(), original.back());
    changed = std::vector<BonePose>{BonePose{glm::quat(2, 3, 4, 5), {1, 2, 3}, {4, 5, 6}}};
    CHECK_FALSE(changed.encoded());
    CHECK(std::as_const(changed).front().rotation.w == 2.0f); // owned EQG keys are already converted
    CHECK(std::as_const(changed).front().scale.z == 6.0f);
    auto owned_copy = changed;
    owned_copy.front().scale.z = 9.0f;
    CHECK(std::as_const(changed).front().scale.z == 6.0f);
    changed.clear();
    CHECK(changed.empty());
    CHECK(original.encoded());
    CHECK(original.decoded_capacity() == 0);
}

TEST_CASE("shared animation frames: sampled clip values match eager poses for looping and clamping") {
    auto source = unpacked_frames();
    source.resize(2);
    source[1].rotation = glm::quat(-1, 2, 3, -4);
    AnimationClip viewed, eager;
    viewed.frame_count = eager.frame_count = 2;
    viewed.frame_ms = eager.frame_ms = 100;
    viewed.tracks.push_back(BoneFrames::from_wld(wld::TrackFrames::from_encoded(
        ImmutableFileBytes::owned(unpacked_bytes(source)), 0, source.size(), false), {0.02f, 0.03f, 0.04f}));
    eager.tracks.push_back({reference_pose(source[0], {0.02f, 0.03f, 0.04f}),
                            reference_pose(source[1], {0.02f, 0.03f, 0.04f})});
    CharacterModel model;
    for (bool loop : {false, true})
        for (float time : {0.0f, 0.025f, 0.1f, 0.15f, 0.35f, -1.0f}) {
            std::vector<BonePose> got, want;
            model.sample_local(viewed, time, loop, got);
            model.sample_local(eager, time, loop, want);
            REQUIRE(got.size() == 1);
            REQUIRE(want.size() == 1);
            same_pose(got.front(), want.front());
        }
    CHECK(std::as_const(viewed).tracks[0].decoded_capacity() == 0);
}

TEST_CASE("shared animation frames: empty views bounds and concurrent reads keep storage immutable") {
    const auto empty = BoneFrames::from_wld({});
    CHECK(empty.empty());
    CHECK(empty.begin() == empty.end());
    CHECK_THROWS_AS(empty.at(0), std::out_of_range);
    const auto source = unpacked_frames();
    const auto keys = BoneFrames::from_wld(wld::TrackFrames::from_encoded(
        ImmutableFileBytes::owned(unpacked_bytes(source)), 0, source.size(), false));
    std::array<std::uint64_t, 4> sums{};
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < sums.size(); ++t)
        threads.emplace_back([&, t] {
            for (int repeat = 0; repeat < 256; ++repeat)
                for (const auto pose : keys) sums[t] += std::bit_cast<std::uint32_t>(pose.translation.x);
        });
    for (auto& thread : threads) thread.join();
    std::uint64_t expected = 0;
    for (const auto pose : keys) expected += std::bit_cast<std::uint32_t>(pose.translation.x);
    for (const auto sum : sums) CHECK(sum == expected * 256);
    CHECK(keys.encoded());
    CHECK(keys.decoded_capacity() == 0);
}

TEST_CASE("shared animation frames: loaded character clips aliases and borrowed hidden bones retain mapped parity") {
    test::ScratchDir dir("animation-library-mapped");
    const auto root = std::filesystem::canonical(dir.path());
    const auto data = test::pfs_fixtures::make_pfs({{"fixture.wld", animation_model_wld()}});
    {
        std::ofstream file(root / "fixture_chr.s3d", std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        REQUIRE(file.good());
    }
    auto build = [&](std::shared_ptr<SharedFileCache> cache) {
        GameDir game(root);
        game.set_shared_file_cache(std::move(cache));
        const auto archive = load_character_archive(game, "fixture_chr.s3d");
        REQUIRE(archive);
        REQUIRE(archive->wlds.size() == 1);
        REQUIRE(archive->wlds[0].errors().empty());
        CharacterLibrary library({archive});
        return library.build("ERM");
    };
    const auto eager = build({});
    auto cache = std::make_shared<SharedFileCache>(SharedFileCache::Options{root / "cache"});
    auto mapped = build(cache); // the source GameDir, archive and library are destroyed before reads below
    cache.reset();
    REQUIRE(mapped.bone_names.size() == 2);
    REQUIRE(mapped.animation_donor == "HUM");
    REQUIRE(mapped.clips.contains("C01A"));
    REQUIRE(mapped.clips.contains("C01"));
    REQUIRE(mapped.clips.contains("D01"));
    CHECK(mapped.clips.at("D01").borrowed);
    const auto& borrowed = std::as_const(mapped).clips.at("D01");
    CHECK(borrowed.tracks[0].front().scale.x == 1.0f);
    CHECK(borrowed.tracks[1].front().scale.x == 0.02f);
    CHECK(mapped.clips.size() == eager.clips.size());
    for (const auto& [name, clip] : std::as_const(mapped).clips) {
        CAPTURE(name);
        REQUIRE(eager.clips.contains(name));
        const auto& expected = eager.clips.at(name);
        REQUIRE(clip.tracks.size() == expected.tracks.size());
        CHECK(clip.frame_ms == expected.frame_ms);
        CHECK(clip.frame_count == expected.frame_count);
        for (std::size_t i = 0; i < clip.tracks.size(); ++i) {
            REQUIRE(clip.tracks[i].size() == expected.tracks[i].size());
            if (!name.empty()) {
                CHECK(clip.tracks[i].mapped());
                CHECK(clip.tracks[i].decoded_capacity() == 0);
            }
            for (std::size_t frame = 0; frame < clip.tracks[i].size(); ++frame)
                same_pose(clip.tracks[i][frame], expected.tracks[i][frame]);
        }
        for (bool loop : {true, false})
            for (float time : {0.0f, 0.025f, 0.15f, 0.35f}) {
                std::vector<BonePose> got, want;
                mapped.sample_local(clip, time, loop, got);
                eager.sample_local(expected, time, loop, want);
                REQUIRE(got.size() == want.size());
                for (std::size_t i = 0; i < got.size(); ++i) same_pose(got[i], want[i]);
            }
    }
    SharedFileCache cleanup({root / "cache"});
    mapped = {};
    CHECK(cleanup.trim(0));
}

TEST_CASE("shared animation tracks: encoded constant compaction preserves logical trajectories and copy-on-write") {
    test::ScratchDir dir("animation-track-encoded-compaction");
    SharedFileCache cache({std::filesystem::canonical(dir.path()) / "cache"});
    wld::TrackFrame constant;
    constant.rotation = glm::quat(2, 0, 0, 0);
    constant.translation = {1, -0.0f, 3};
    auto encoded = [&](std::size_t count) {
        const std::vector<wld::TrackFrame> frames(count, constant);
        auto bytes = cache.store(unpacked_bytes(frames));
        REQUIRE(bytes.mapped());
        return AnimationTrack::from_wld(wld::TrackFrames::from_encoded(std::move(bytes), 0, count, false));
    };
    CharacterModel model;
    AnimationClip clip;
    clip.frame_ms = 70;
    clip.frame_count = 9;
    clip.tracks.push_back(encoded(4));
    clip.tracks.push_back(encoded(7));
    BonePose moving = reference_pose(constant);
    moving.translation.x = 9;
    clip.tracks.push_back({reference_pose(constant), moving});
    model.clips["L01"] = clip;
    model.clips["L01A"] = clip;
    const auto before = model;
    model.compact_animations();
    const auto& compacted = model.clips.at("L01");
    CHECK(compacted.tracks[0].mapped());
    CHECK(compacted.tracks[1].mapped());
    CHECK(compacted.tracks[0].stored_keys() == 1);
    CHECK(compacted.tracks[0].size() == 4);
    CHECK(compacted.tracks[1].size() == 7);
    CHECK(compacted.tracks[0].storage_id() == compacted.tracks[1].storage_id());
    CHECK(compacted.tracks[0].decoded_capacity() == 0);
    for (bool loop : {false, true})
        for (float time : {-1.0f, 0.0f, 0.035f, 0.07f, 0.135f, 0.28f, 0.49f, 0.63f, 1.5f}) {
            std::vector<BonePose> got, want;
            model.sample_local(compacted, time, loop, got);
            before.sample_local(before.clips.at("L01"), time, loop, want);
            REQUIRE(got.size() == want.size());
            for (std::size_t i = 0; i < got.size(); ++i) same_pose(got[i], want[i]);
        }
    auto changed = compacted.tracks[0];
    changed.push_back(changed.front());
    CHECK_FALSE(changed.encoded());
    CHECK(changed.size() == 5);
    CHECK(compacted.tracks[0].mapped());
    CHECK(compacted.tracks[0].size() == 4);
    for (std::size_t i = 0; i < changed.size(); ++i) same_pose(changed[i], reference_pose(constant));
    auto moved = std::move(changed);
    CHECK(changed.empty());
    CHECK(moved.size() == 5);
    CHECK(model.clips.at("L01A").tracks[0].mapped());
}

TEST_CASE("shared animation tracks: private encoded owners are accounted once across aliases and views") {
    const auto source = unpacked_frames();
    const auto raw = unpacked_bytes(source);
    const auto owner = ImmutableFileBytes::owned(raw);
    const auto first = AnimationTrack::from_wld(wld::TrackFrames::from_encoded(owner, 0, source.size(), false));
    const auto second = AnimationTrack::from_wld(wld::TrackFrames::from_encoded(owner, 0, 1, false));
    const auto alias = first;
    AnimationTrack::StorageSet unique, first_only, second_only;
    const auto first_bytes = first.storage_bytes(unique);
    CHECK(first_bytes >= raw.size());
    CHECK(alias.storage_bytes(unique) == 0);
    const auto total = first_bytes + second.storage_bytes(unique);
    CHECK(total < first.storage_bytes(first_only) + second.storage_bytes(second_only));
    CHECK(first.decoded_capacity() == 0);
    CHECK(second.decoded_capacity() == 0);
}

TEST_CASE("shared animation tracks: singleton and multi-key constants intern safely in either order") {
    test::ScratchDir dir("animation-track-constant-intern-order");
    SharedFileCache cache({std::filesystem::canonical(dir.path()) / "cache"});
    wld::TrackFrame constant;
    constant.rotation = glm::quat(2, 0, 0, 0);
    constant.translation = {1, -0.0f, 3};
    auto encoded = [&](std::size_t count) {
        auto bytes = cache.store(unpacked_bytes(std::vector<wld::TrackFrame>(count, constant)));
        REQUIRE(bytes.mapped());
        return AnimationTrack::from_wld(wld::TrackFrames::from_encoded(std::move(bytes), 0, count, false));
    };
    for (bool singleton_first : {true, false}) {
        CAPTURE(singleton_first);
        CharacterModel model;
        AnimationClip clip;
        clip.frame_ms = 70;
        clip.frame_count = 7;
        clip.tracks.push_back(encoded(singleton_first ? 1 : 4));
        clip.tracks.push_back(encoded(singleton_first ? 4 : 1));
        model.clips["L01"] = clip;
        model.clips["L01A"] = clip;
        const auto before = model;
        model.compact_animations();
        const auto& compacted = model.clips.at("L01");
        const auto four = singleton_first ? 1u : 0u;
        const auto one = singleton_first ? 0u : 1u;
        CHECK(compacted.tracks[four].size() == 4);
        CHECK(compacted.tracks[one].size() == 1);
        CHECK(compacted.tracks[four].stored_keys() == 1);
        CHECK(compacted.tracks[one].stored_keys() == 1);
        CHECK(compacted.tracks[four].storage_id() == compacted.tracks[one].storage_id());
        CHECK(compacted.tracks[four].mapped());
        CHECK(compacted.tracks[four].decoded_capacity() == 0);
        CHECK(compacted.frame_count == 7);
        for (const auto& [name, current] : model.clips) {
            for (bool loop : {false, true})
                for (float time : {0.0f, 0.035f, 0.07f, 0.135f, 0.28f, 0.49f, 0.63f, 1.5f}) {
                    std::vector<BonePose> got, want;
                    REQUIRE_NOTHROW(model.sample_local(current, time, loop, got));
                    before.sample_local(before.clips.at(name), time, loop, want);
                    REQUIRE(got.size() == want.size());
                    for (std::size_t i = 0; i < got.size(); ++i) same_pose(got[i], want[i]);
                }
        }
        auto changed = compacted.tracks[four];
        REQUIRE_NOTHROW(changed.push_back(changed.front()));
        CHECK(changed.size() == 5);
        CHECK_FALSE(changed.encoded());
        for (std::size_t i = 0; i < changed.size(); ++i) same_pose(changed[i], reference_pose(constant));
        CHECK(model.clips.at("L01A").tracks[four].size() == 4);
        CHECK(model.clips.at("L01A").tracks[four].mapped());
        CHECK(model.clips.at("L01A").tracks[four].stored_keys() == 1);
    }
}

TEST_CASE("shared animation tracks: character resource indices survive deferred payload release and reload") {
    struct RestoreRetainBudget {
        std::size_t previous = wld::WldFile::retain_budget();
        ~RestoreRetainBudget() { wld::WldFile::set_retain_budget(previous); }
    } restore;
    wld::WldFile::set_retain_budget(0);
    test::ScratchDir dir("animation-character-deferred-mapped");
    const auto root = std::filesystem::canonical(dir.path());
    const auto raw = test::pfs_fixtures::make_pfs({{"fixture.wld", animation_model_wld()}});
    {
        std::ofstream file(root / "fixture_chr.s3d", std::ios::binary);
        file.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        REQUIRE(file.good());
    }
    GameDir game(root);
    game.set_shared_file_cache(std::make_shared<SharedFileCache>(SharedFileCache::Options{root / "cache"}));
    const auto archive = load_character_archive(game, "fixture_chr.s3d");
    REQUIRE(archive);
    REQUIRE(archive->wlds.size() == 1);
    const auto& deferred = archive->wlds[0];
    CHECK_FALSE(deferred.materialized());
    CharacterLibrary library({archive});
    CHECK_FALSE(deferred.materialized());
    const auto first = library.build("ERM");
    CHECK_FALSE(deferred.materialized());
    const auto again = library.build("ERM");
    CHECK_FALSE(deferred.materialized());
    CHECK(deferred.decode_stats().decodes >= 2);
    CHECK(deferred.decode_stats().releases >= 2);
    for (const auto& [name, clip] : again.clips) {
        const auto& previous = first.clips.at(name);
        REQUIRE(clip.tracks.size() == previous.tracks.size());
        if (!name.empty())
            for (const auto& track : clip.tracks) {
                CHECK(track.mapped());
                CHECK(track.decoded_capacity() == 0);
            }
        for (bool loop : {false, true})
            for (float time : {0.0f, 0.025f, 0.15f, 0.35f}) {
                std::vector<BonePose> got, want;
                again.sample_local(clip, time, loop, got);
                first.sample_local(previous, time, loop, want);
                REQUIRE(got.size() == want.size());
                for (std::size_t i = 0; i < got.size(); ++i) same_pose(got[i], want[i]);
            }
    }
}
