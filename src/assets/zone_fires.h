#pragma once

// The zone's fires, found in its data for the modern lighting's procedural flames (render/fire_renderer.h,
// docs/graphics.md "Fire"). The original draws a flame one of three ways, by the zone's era:
//   - a flame mesh on a placed object or door: crossed quads with an animated additive texture (FIRE1_MDF's
//     fire1..4.bmp and its blue, green, red and violet kin), the old world's and Velious's torches, lamps,
//     braziers and campfires;
//   - a particle cloud on a placed object's skeleton node (0x34, Zone::particle_fixtures), Luclin onwards: flame
//     sprites GENG*.DDS / GENPOP*.DDS, usually with a smoke cloud beside them;
//   - an EQG zone's environment emitters (<zone>_EnvironmentEmitters.txt over environmentemittersnew.edd): the
//     same kind of cloud at a point, flame sprites FlameD.dds, new_flame.dds, new_fire.dds and the like.
// What is not a flame though it is named like one stays: the logs, coals and holders (FIREWOOD, COALS, TORCHTOP),
// the fireplaces' stones, Takish's red crystal torches (TAKTORCH01), YKFLAME01 (black: its flame is the cloud on
// it) and the smoke.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "assets/model.h"
#include "assets/spell_effects.h"

namespace hailstone { class GameDir; }

namespace hailstone::assets {

struct Zone;

// The colour a flame burns with: ordinary fire, or a magic one.
enum class FlameHue : std::uint8_t { None, Fire, Blue, Green, Red, Violet };

// A material that is a flame (by its name, as the survey of every zone's materials found them), or None.
FlameHue flame_material_hue(const Material& m);
// An emitter definition whose sprites are flames (by its texture, and not lava), or None.
FlameHue flame_emitter_hue(const EmitterDefinition& d);

// A flame's box: the triangles of one flame (a torch's crossed quads; each candle of a candelabra its own).
struct FlameBox {
    glm::vec3 min{0.0f}, max{0.0f};
    FlameHue hue = FlameHue::Fire;
};

// The mesh's flames in its own space: its flame materials' triangles, those whose boxes touch (within a quarter
// unit) taken as one flame.
std::vector<FlameBox> mesh_flames(const Mesh& mesh);

// A flame where it stands, in world units.
struct ZoneFire {
    glm::vec3 base{0.0f}; // the middle of its foot
    float width = 1.0f;
    float height = 2.0f;
    FlameHue hue = FlameHue::Fire;
};

// A mesh flame placed by `transform` (its box's corners into the world: the foot at their lowest).
ZoneFire place_flame(const FlameBox& box, const glm::mat4& transform);
// The flames of every placement whose object has flame meshes.
std::vector<ZoneFire> placement_fires(const Zone& zone);

// The flames of `a` and `b` as one list, those standing in another's flame merged into the bigger: a classic zone's
// environment emitters burn on its torches' flame meshes (frozenshadow), a fireplace's clouds lie one in the other.
std::vector<ZoneFire> merge_fires(std::vector<ZoneFire> a, const std::vector<ZoneFire>& b);
// A light for each fire with none of the zone's lights within `reach` (EQG zones with flames and no lights: argath,
// commonlands...), added to Zone::lights in its hue. Returns how many were added.
std::size_t add_fire_lights(Zone& zone, const std::vector<ZoneFire>& fires, float reach = 30.0f);

// One line of <zone>_EnvironmentEmitters.txt: Name^EmitterDefIdx^X^Y^Z^[AlwaysVisible^]Lifespan, the position in
// the zone's own frame.
struct EnvironmentEmitter {
    std::string name;
    std::int32_t definition = 0; // index into environmentemittersnew.edd
    glm::vec3 position{0.0f};
};
// The file's emitters; the header line and malformed lines are skipped.
std::vector<EnvironmentEmitter> parse_environment_emitters(std::string_view text);
// Adds the zone's environment emitters (when it has the file) to Zone::particle_definitions and particle_fixtures:
// the EDD's records after those already there, one fixture per emitter whose index the EDD has, pointing up.
// Returns how many were added. A revamped zone (is_revamped_zone_name) loaded as S3D (not Zone::from_eqg) takes none:
// the file is the live .eqg's.
std::size_t add_environment_emitters(const GameDir& game, Zone& zone);

} // namespace hailstone::assets
