#pragma once

// A model's named points (spell points): where spell particles attach to a
// character.
//
// eqgame's ActorClient::InitSpellPoints @0x40a3d0 looks up ten points by name
// through the actor (vtable +0x184) into ActorClient +0x148 (spawn +0xfec),
// which SpellEffect_SpawnStageEmitters reads as 0x58cd90(n):
// 0 SPELLPOINT_DEFAULT, 1 CHEST, 2 HEAD, 3 LHAND, 4 RHAND, 5 LFOOT, 6 RFOOT,
// 7 LEYE, 8 REYE, 9 MOUTH.
//
// The model definition holds the points (0xa4-byte records: name[64],
// bone[64], offset xyz, rotation xyz in 512ths, scale xyz):
// - EQG models: `<model>.pts` beside the mesh in its archive ("EQPT", u32
//   count, u32 version 1, the records), read by the EQGM loader @0x100624e0;
// - WLD models: built from the skeleton's bone names by 0x1004c690, with the
//   mouth's offsets for a model without HEHEAD from Resources\SPOffsets.ini
//   (section = the model code, 0x1001ef90).
// A point's bone is the first with exactly its name (0x1006f5c0); its
// matrix is 0x1006f370's (point_matrix).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::assets {

struct CharacterModel;

struct ModelPoint {
    std::string name; // "SPELLPOINT_LHAND"
    std::string bone; // the bone's exact name
    glm::vec3 offset{0.0f};
    glm::vec3 rotation{0.0f}; // heading, pitch, roll; 512 per turn
    glm::vec3 scale{1.0f};
};

// The ten spell points in InitSpellPoints' order.
inline constexpr std::array<std::string_view, 10> kSpellPointNames = {
    "SPELLPOINT_DEFAULT", "SPELLPOINT_CHEST", "SPELLPOINT_HEAD", "SPELLPOINT_LHAND", "SPELLPOINT_RHAND",
    "SPELLPOINT_LFOOT",   "SPELLPOINT_RFOOT", "SPELLPOINT_LEYE", "SPELLPOINT_REYE",  "SPELLPOINT_MOUTH",
};

// A .pts file. Throws std::runtime_error for a missing magic or a short file;
// a version other than 1 gives no points (the loader's rule).
std::vector<ModelPoint> parse_pts(std::span<const std::byte> data);

// A key of Resources\SPOffsets.ini: (section, key) -> value, if present.
using SpOffsetLookup = std::function<std::optional<float>(std::string_view section, std::string_view key)>;

// 0x1004c690's points for a WLD skeleton (`bones` in the model's order; the
// code is the first three letters of bone 0's name):
// - DEFAULT on bone 0;
// - CHEST on <code>CHEST_POINT_TRACK, else <code>CH_TRACK, else none;
// - HEAD and MOUTH on <code>HEHEAD_TRACK (the mouth at (0.08, 0.6, 0),
//   turned (128, 0, 128)), else on <code>HEAD_POINT_TRACK with the mouth's
//   XOffset .. ZScale from SPOffsets.ini [<code>] (defaults 0.6, 0, 0, 0, 0,
//   0, 1, 1, 1), else neither;
// - LHAND / RHAND on <code>L_POINT_TRACK / R_POINT_TRACK, else
//   <code>SPELL_POINT_TRACK, else none;
// - LFOOT / RFOOT on <code>BOFOOTL_TRACK / BOFOOTR_TRACK, else <code>BO_L_TRACK /
//   BO_R_TRACK, else none.
// WLD models have no eye points.
std::vector<ModelPoint> wld_model_points(std::span<const std::string> bones, const SpOffsetLookup& offsets);

// 0x1006f370: the point's matrix in its bone's space (column vectors):
// scale, then the rotation from (heading a, pitch b, roll c) with the table
// sine and cosine of (a, -b, c), then the offset.
glm::mat4 point_matrix(const ModelPoint& p);

// The first point named `name`, or nullptr.
const ModelPoint* find_point(std::span<const ModelPoint> points, std::string_view name);

// One actor emitter of a model (`<model>.prt` beside its mesh: "PTCL", u32
// count, u32 version 1..5, records of 0x5c / 0x60 / 0x64 / 0x68 / 0x6c bytes
// that the EQGM loader @0x100624e0 widens to 0x6c; 0x1006bdb0 copies them
// into the definition's emitter list). The emitter indices are
// ActorEmittersNew.edd's.
struct ModelEmitter {
    std::uint32_t emitter = 0;     // +0x00
    std::string point;             // +0x04 char[64]: the model point it sits on
    std::int32_t trigger = 0;      // +0x44: 0 always on while the actor stands (0x1006b130 / 0x1006b290);
                                   //        1 started by an animation (not drawn here)
    std::int32_t animation = 0;    // +0x48: the animation that starts a triggered one
    std::int32_t unknown_4c = 0, unknown_50 = 0; // +0x4c, +0x50
    std::int32_t delay_ms = 0;     // +0x54: probably the delay into the animation
    std::int32_t duration_ms = 0;  // +0x58 (5000 in every NMS file; version 1: 0)
    std::int32_t floor = 0;        // +0x5c: 1 drops the emitter to the floor below the point (0x1006b5b0)
    std::int32_t variant = -1;     // +0x60: the actor variant it belongs to (-1 all; 0x1006b480);
                                   //        always-on emitters are made only below 1
    std::int32_t unknown_64 = 0;   // +0x64
    std::uint32_t alternate = 0;   // +0x68: the emitter used instead when CParticleSystem vtable +0xb4 says so
                                   //        (version 4 and below: the emitter itself)
};

// A .prt file. Throws std::runtime_error for a missing magic or a short file;
// an unknown version gives no emitters (the loader's rule).
std::vector<ModelEmitter> parse_prt(std::span<const std::byte> data);

// The ten spell points of each model, resolved to bones once: an EQG model's
// own points, a WLD model's from wld_model_points.
class SpellPoints {
public:
    struct Model {
        std::array<int, 10> bone{};        // the bone each point hangs from
        std::array<glm::mat4, 10> local{}; // point_matrix, in that bone's space
        std::uint16_t mask = 0;            // bit n: the model has point n
    };
    explicit SpellPoints(SpOffsetLookup offsets = {}) : offsets_(std::move(offsets)) {}
    // The DLL keeps a point whose bone the skeleton lacks (bone index -1,
    // 0x1006f5c0); where it then sits was not traced, so such a point is
    // left out (no shipped .pts names a missing bone).
    const Model& of(const CharacterModel& model);

private:
    // By the model's serial, not its address: a model freed and another made at its address must not share an
    // entry. Cleared when it reaches kMaxModels, so models the session dropped do not pile up.
    static constexpr std::size_t kMaxModels = 4096;
    SpOffsetLookup offsets_;
    std::unordered_map<std::uint64_t, Model> models_;
};

} // namespace hailstone::assets
