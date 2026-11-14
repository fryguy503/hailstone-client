#include <doctest/doctest.h>

#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>

#include "assets/model.h"
#include "render/material_state.h"

using namespace hailstone;

// The depth pre-pass draws the opaque pass's materials and takes its alpha from the texture, or 1
// (shaders/zone_depth.frag). The effects whose alpha zone.frag computes another way must stay out of that pass.
TEST_CASE("depth pre-pass: the effects with an alpha of their own are not in the opaque pass") {
    using assets::EqgShader;
    for (EqgShader shader : {EqgShader::Water, EqgShader::LavaH, EqgShader::WaterFall, EqgShader::WldDetail}) {
        assets::Material m;
        m.eqg_shader = shader;
        CHECK(render::material_state(m).pass != 0);
    }
}

TEST_CASE("depth pre-pass: the effects it draws are in the opaque pass") {
    using assets::EqgShader;
    for (EqgShader shader : {EqgShader::None, EqgShader::Chroma, EqgShader::Terrain, EqgShader::Lava, EqgShader::Lava2}) {
        assets::Material m;
        m.eqg_shader = shader;
        CHECK(render::material_state(m).pass == 0);
    }
}

namespace {

std::string read_shader(const char* name) {
    std::ifstream in(std::string(HAILSTONE_SOURCE_DIR "/shaders/") + name, std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The calls in a shader's code (not its comments) to a helper that decides a discard: the *_discard helpers
// (lighting.glsl reveal_discard), the alpha test (coverage.glsl alpha_test) and a level of detail's sample mask
// (coverage.glsl lod_fade_samples).
std::set<std::string> discard_helpers(const std::string& source) {
    const std::string code = std::regex_replace(source, std::regex(R"(//[^\n]*)"), "");
    std::set<std::string> found;
    const std::regex call(R"(\b(\w+_discard|alpha_test|lod_fade_samples)\s*\()");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), call); it != std::sregex_iterator(); ++it)
        found.insert((*it)[1].str());
    return found;
}

} // namespace

// The colour pass over the pre-pass tests depth equal and writes none: a pixel zone.frag discards that the depth
// shader keeps would hide what is behind it (the isometric camera's cut walls: a hole of clear colour).
TEST_CASE("depth pre-pass: the depth shader discards where zone.frag does") {
    const auto colour = discard_helpers(read_shader("zone.frag"));
    const auto depth = discard_helpers(read_shader("zone_depth.frag"));
    CHECK(colour.count("reveal_discard") == 1);
    // A level of detail's fade: the same samples in both (coverage.glsl lod_fade_samples).
    CHECK(colour.count("lod_fade_samples") == 1);
    // A neighbour's part of the land: the same side of the blend in both (coverage.glsl land_blend_discard).
    CHECK(colour.count("land_blend_discard") == 1);
    for (const auto& helper : colour) {
        INFO("zone.frag calls " << helper << ", zone_depth.frag does not");
        CHECK(depth.count(helper) == 1);
    }
}
