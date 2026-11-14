#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "assets/flora.h"
#include "assets/model.h"
#include "assets/pfs.h"
#include "assets/regions.h"
#include "assets/spell_effects.h"

namespace hailstone { class GameDir; }

namespace hailstone::assets {

// A particle emitter standing in the zone: a placed object's particle node.
struct ParticleFixture {
    std::uint32_t definition = 0; // index into Zone::particle_definitions (>= 1)
    // The node's world matrix (placement x the skeleton's rest pose down to the
    // node): its translation is the emitter origin and its axes the owner
    // basis the emitter sprays along.
    glm::mat4 transform{1.0f};
};

// Everything needed to draw a zone's static world.
struct Zone {
    std::string name;
    std::vector<Mesh> terrain;                       // zone geometry, usually one mesh per region
    std::unordered_map<std::string, Mesh> objects;   // actor name (upper case, as in the WLD) -> mesh
    std::vector<Placement> placements;               // where the objects stand
    // Replacement models (assets/model_overrides.h) drawn in place of the objects of the same name; the look only:
    // collision, picking and the rest keep `objects`.
    std::unordered_map<std::string, Mesh> draw_objects;
    std::vector<PointLight> lights;
    // Particle emitters the placed objects carry (classic zones: 0x34 clouds on
    // skeleton nodes, assets/wld_particles.h): the definitions, the first of a
    // name winning as the DLL's named-resource reuse does, and one fixture per
    // particle node of each placement. Index 0 is an unused "None" record, as
    // in the EDD files, so emitter indices start at 1.
    std::vector<EmitterDefinition> particle_definitions;
    std::vector<ParticleFixture> particle_fixtures;
    // The zone WLD's 0x35 global ambient, byte / 255 (0 for EQG zones). The
    // original adds it into the region shaders' SpecialAmbient.
    glm::vec3 global_ambient{0.0f};
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
    // Water, lava, PvP, zone lines... from the zone WLD's BSP (assets/regions.h).
    ZoneRegions regions;
    // An EQG heightmap zone's radial flora (assets/flora.h; /showgrass): none elsewhere.
    ZoneFlora flora;

    // The archives the zone came from, searched in order for texture files.
    std::vector<std::shared_ptr<const PfsArchive>> archives;
    // Textures made by the loader (baked heightmap tiles), looked up before
    // the archives; keys are lower case.
    std::unordered_map<std::string, std::vector<std::byte>> generated_files;
    std::optional<std::vector<std::byte>> read_file(std::string_view name) const;
};

// A load's coarse cancel check, asked before the load and between its files and big steps (a background load whose
// result is no longer wanted). True stops the load with ZoneLoadCancelled.
using ZoneLoadCancel = std::function<bool()>;
struct ZoneLoadCancelled : std::runtime_error {
    ZoneLoadCancelled() : std::runtime_error("zone load cancelled") {}
};

// Loads a classic (S3D/WLD) zone: <name>.s3d (terrain, objects.wld placements,
// lights.wld) and <name>_obj.s3d (object meshes), plus _obj2 when present.
// Throws std::runtime_error when the zone's archives are missing or malformed.
Zone load_classic_zone(const GameDir& game, std::string_view name, const ZoneLoadCancel& cancel = {});

// Loads an EQG zone: <name>.eqg with its binary (EQGZ) or heightmap (EQTZP)
// zone declaration (assets/eqg.h, docs/formats/eqg.md). Throws
// std::runtime_error when the archive or its declaration is missing or malformed.
Zone load_eqg_zone(const GameDir& game, std::string_view name, const ZoneLoadCancel& cancel = {});

// True when the zone loads as EQG: the original tries <name>.EQG first and
// falls back to <name>.s3d (CDisplay::StartWorldDisplay @0x49b200). A change:
// arena, bazaar, highpasshold, lavastorm and nektulos load as S3D when there
// is one, since a live asset folder's .eqg of that name is a revamped zone.
bool is_eqg_zone(const GameDir& game, std::string_view name);
// arena, bazaar, highpasshold, lavastorm, nektulos: zones the live client revamped under the RoF2 name. With a live
// asset folder and no .s3d of theirs, load_zone refuses the live .eqg unless the server's own files have it or the
// RevampedZoneTest passes.
bool is_revamped_zone_name(std::string_view name);

// Asked with a live folder's revamped .eqg zone, loaded, when load_zone would otherwise refuse it: true when it is
// the zone the server runs (game/revamped_zone.h tests the server's safe point against its floor), and load_zone
// returns it. Empty: refused, as with no word from the server (a neighbour zone loaded ahead, the viewers).
using RevampedZoneTest = std::function<bool(const Zone&)>;

// The zone as the original picks it: EQG when <name>.eqg exists, else classic (with is_eqg_zone's and
// is_revamped_zone_name's changes).
Zone load_zone(const GameDir& game, std::string_view name, const ZoneLoadCancel& cancel = {},
               const RevampedZoneTest& revamped_test = {});

} // namespace hailstone::assets
