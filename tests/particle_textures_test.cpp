#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "core/byte_io.h"
#include "core/game_dir.h"
#include "render/particle_textures.h"
#include "assets/spell_effects.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {
// A 2x1 24-bit BMP of two colours (B, G, R each).
std::vector<std::byte> bmp2(std::uint8_t b0, std::uint8_t g0, std::uint8_t r0, std::uint8_t b1, std::uint8_t g1,
                            std::uint8_t r1) {
    ByteWriter w;
    w.le<std::uint8_t>('B');
    w.le<std::uint8_t>('M');
    w.le<std::uint32_t>(14 + 40 + 8);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(14 + 40);
    w.le<std::uint32_t>(40);
    w.le<std::int32_t>(2);
    w.le<std::int32_t>(1);
    w.le<std::uint16_t>(1);
    w.le<std::uint16_t>(24);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(8);
    w.zeros(16);
    for (std::uint8_t v : {b0, g0, r0, b1, g1, r1}) w.le(v);
    w.zeros(2); // row padding to 4 bytes
    return w.take();
}
void write(const std::filesystem::path& p, const std::vector<std::byte>& d) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}
} // namespace

TEST_CASE("Particle textures: WLD bitmap sequences wrap without cropping EDD atlas frames") {
    assets::EmitterDefinition definition;
    definition.texture = "atlas.dds";
    definition.frames = 16;
    auto atlas = render::particle_texture_frame(definition, 7);
    CHECK(atlas.texture == "atlas.dds");
    CHECK(atlas.grid == glm::uvec2(4, 4));
    CHECK(atlas.atlas_frame == 7);
    definition.texture_frames = {"first.bmp", "second.bmp", "third.bmp", "fourth.bmp", "fifth.bmp"};
    definition.frames = 5;
    auto sequence = render::particle_texture_frame(definition, 7);
    CHECK(sequence.texture == "third.bmp");
    CHECK(sequence.grid == glm::uvec2(1));
    CHECK(sequence.atlas_frame == 0);
    CHECK(render::particle_texture_frame(definition, 5).texture == "first.bmp");
}

TEST_CASE("Particle textures: the zone's copy, else the effect folders in order; the colour of its texels") {
    test::ScratchDir dir("particle_textures");
    write(dir.path() / "EnvEmitterEffects" / "spark.bmp", bmp2(0, 0, 200, 0, 0, 100));  // red
    write(dir.path() / "ActorEffects" / "spark.bmp", bmp2(200, 0, 0, 200, 0, 0));       // blue: not reached
    write(dir.path() / "ActorEffects" / "blue.bmp", bmp2(255, 0, 0, 0, 0, 0));
    GameDir game(dir.path());
    auto spark = render::load_particle_texture("spark.bmp", std::nullopt, game);
    REQUIRE(spark.image);
    CHECK(spark.image->width == 2);
    CHECK(spark.color.r == doctest::Approx(1.0f));
    CHECK(spark.color.b == doctest::Approx(0.0f));
    auto blue = render::load_particle_texture("blue.bmp", std::nullopt, game);
    CHECK(blue.color.b == doctest::Approx(1.0f));
    CHECK(blue.color.r == doctest::Approx(0.0f));
    // The zone's own copy comes first.
    auto zone = render::load_particle_texture("spark.bmp", bmp2(0, 255, 0, 0, 255, 0), game);
    CHECK(zone.color.g == doctest::Approx(1.0f));
    CHECK(zone.color.r == doctest::Approx(0.0f));
    // None anywhere: no image, white.
    auto none = render::load_particle_texture("nothing.dds", std::nullopt, game);
    CHECK_FALSE(none.image);
    CHECK(none.color == glm::vec3(1.0f));
}

TEST_CASE("Particle textures: a missing, unnamed or over-limit one is not drawn, not drawn white") {
    // Spell effect 226's impact emitter 1351 names spear2.dds, which no install has: RoF2 puts nothing on the caster.
    const std::unordered_map<std::string, std::uint32_t> slots{
        {"bloba.dds", 3},
        {"spear2.dds", render::kMissingParticleTexture},
        {"pastlimit.dds", render::kMissingParticleTexture}};
    const std::unordered_set<std::string> loading{"zapmuza.dds"};
    CHECK(render::particle_texture_slot("bloba.dds", slots, loading) == std::optional<std::uint32_t>(3u));
    CHECK_FALSE(render::particle_texture_slot("spear2.dds", slots, loading));
    CHECK_FALSE(render::particle_texture_slot("zapmuza.dds", slots, loading)); // not up yet
    // An empty name is not drawn either (the original finds no file for it), nor is one that came past the texture
    // limit (prepare records it as missing) or one never put up.
    CHECK_FALSE(render::particle_texture_slot("", slots, loading));
    CHECK_FALSE(render::particle_texture_slot("pastlimit.dds", slots, loading));
    CHECK_FALSE(render::particle_texture_slot("unasked.dds", slots, loading));
}

TEST_CASE("Particle textures: one past the texture limit, or that did not decode, gets no slot") {
    // Slot 0 is the white texture; the next decoded one takes the next slot while there is room.
    CHECK(render::particle_upload_slot(true, 1, 4) == 1u);
    CHECK(render::particle_upload_slot(true, 3, 4) == 3u);
    CHECK(render::particle_upload_slot(true, 4, 4) == render::kMissingParticleTexture); // past Hailstone's cap
    CHECK(render::particle_upload_slot(false, 1, 4) == render::kMissingParticleTexture);
}
