#pragma once

// EQG character models: weighted meshes (EQGS .mds, EQGM .mod with bones)
// and timed bone tracks (EQGA .ani), converted to CharacterModel. Format
// notes: docs/formats/characters.md ("EQG models"). The layouts follow OpenEQ
// crates/openeq-assets/src/character/modern.rs (MIT OR Apache-2.0); see
// THIRD_PARTY.md.
//
// Where the files are: Resources/OnDemandResources.txt lines
// "archive^member^name^type" (EQGS / EQGM meshes "<CODE>_ACTORDEF", EQGA
// animations "<ANIM>_<CODE>"), else <code>.eqg with <code>.mds / <code>.mod
// and <anim>_<code>.ani members. A model without its own animation uses the
// race table's fallback model's ("HNM" -> "HUM" -> "DKM",
// game::animation_fallback_code).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "assets/character.h"
#include "assets/pfs.h"
#include "core/game_dir.h"

namespace hailstone::assets {

namespace eqg_character {

struct Bone {
    std::string name;
    std::int32_t next = -1;   // next sibling
    std::uint32_t children = 0;
    std::int32_t child = -1;  // first child
    BonePose bind;
};

struct Piece {
    std::string name;
    std::vector<SkinnedVertex> vertices; // model (bind) space
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<std::int32_t> triangle_materials; // -1 = none
};

struct MaterialInfo {
    std::string name;
    std::string shader;
    std::string diffuse; // e_TextureDiffuse0
    EffectParams effect;
    struct SurfaceProperties {
        bool diffuse = false, normal = false, environment = false;
        bool shininess = false, env_strength = false;
    } surface_properties;
};

// A parsed .mds / .mod character mesh.
struct MeshFile {
    bool mds = false;
    std::uint32_t version = 0;
    std::vector<MaterialInfo> materials;
    std::vector<Bone> bones;
    std::vector<Piece> pieces;
};

// A Hero's Forge attachment's mesh (EqgCharacterStore::armor_part_mesh) and
// whether append_pieces draws its materials' authored surface maps. The flag
// lives here, not in the mesh, so a direct part shares the cached mesh.
struct ArmorPart {
    std::shared_ptr<const MeshFile> mesh;
    bool surface_maps = false;
    explicit operator bool() const { return mesh != nullptr; }
};

// Throws std::runtime_error when malformed.
MeshFile parse_mesh(std::span<const std::byte> data, std::string_view name);
// Preserve an EQG attachment's bind-space vertices and influences on a WLD
// body. The resulting mesh uses CharacterMesh's attachment bone palette.
bool bind_part_to_wld(const MeshFile& file, std::string_view body_code,
                      std::span<const std::string> body_bones, int anchor,
                      const glm::mat4& adjustment, CharacterMesh& mesh);

struct Track {
    std::string bone;
    std::vector<std::uint32_t> times_ms;
    std::vector<BonePose> poses;
};
struct AnimationFile {
    std::vector<Track> tracks;
};
AnimationFile parse_animation(std::span<const std::byte> data);

// An EQGL (.lay) material layer: a named texture set for one material of a
// new-style model, "C_<CODE>_S<variant>_M<material + 1>". EQG_LoadFileByMagic
// @0x10065c00 -> 0x100645f0 (EQGL v1-4: entries of 10 / 16 / 7 / 8 u32s)
// -> 0x10016490 and siblings: entry[0] the name, entry[1..5] texture names
// (-1 none), each one's role from the character five from its end ('C'
// diffuse, 'N' normal, 'E' environment; "X_C.DDS").
struct Layer {
    std::string name;
    std::string diffuse;
    std::string normal;
    std::string environment;
    // SetLayer @0x10048130 replaces the named properties with these raw
    // layer values. V3 supplies environment strength 1; V4 stores both.
    // Versions 1/2 have different tails; their scalar layouts remain open.
    std::optional<float> shininess;
    std::optional<float> env_strength;
};
// Throws std::runtime_error when malformed.
std::vector<Layer> parse_layers(std::span<const std::byte> data);
// "C_<CODE>_S<variant %02d>_M<material + 1 %02d>", the name
// CHierarchicalActor::SwapNpcMaterialsNewModel @0x100433d0 looks up for
// material `material` (file order) of a new-style model.
std::string layer_name(std::string_view code, std::uint32_t variant, std::size_t material);

// The classic clip code of an EQG animation tag: its sequence's old-style
// number (the sequence table at 0xaae3d8 and the new-to-old map at
// 0xaaf428: "STND" -> P01, "WALK" -> L01, "SLPR" and "BLPR" -> C05), or
// empty for a tag the table lacks or one without an old-style number.
std::string_view clip_code(std::string_view prefix);
// The weapon styles of the track names (0xaaf410): base, one-handed,
// two-handed, dual wield, staff (ST2H), mounted.
inline constexpr std::string_view kTrackStyles[] = {"BA", "1H", "2H", "DW", "ST", "MT"};
// Whether an animation name (the member name before "_<model code>.ani") is
// one InitAnimationSoundTracks @0x407dc0 binds: "<tag>_<style>_<n>" with a
// tag of the sequence table, a style of kTrackStyles and n 1..4
// (FindAnimationTrackForModel @0x406830 looks them up by that name).
bool is_track_name(std::string_view name);
// The classic clip codes an EQG model answers: each old-style number's
// sequence (the old-to-new map at 0xaaf5c8) in the base style, variant 1
// (C05 -> SLPR_BA_1, O02 -> IDLE_BA_1, O03 -> SIDL_BA_1), when it has it.
std::map<std::string, std::string, std::less<>> classic_aliases(
    const std::function<bool(std::string_view)>& has_track);

// The pieces to draw for a body variant and head variant: the body
// <code><body> (else <code>, <code>00, the first piece) and the head
// <code>HE<head> (else <code>HE00) when the file splits them.
std::vector<std::size_t> select_pieces(const MeshFile& mesh, std::string_view code, std::uint32_t body,
                                       std::uint32_t head);
// Each bone's parent from the child / next lists, leniently for an attached part (a bad child index ends its list, a
// bone listed twice keeps its first parent; -1 for a root), or nullopt when the hierarchy has a cycle. A list is
// followed for at most the bone count, however many children a bone claims.
std::optional<std::vector<std::int32_t>> part_bone_parents(const MeshFile& mesh);
// Appends pieces as parts grouped by material. `surface_maps`: the materials
// carry their authored normal / environment maps and highlights (Hero's Forge
// armor, ArmorPart); ordinary EQG bodies and held parts keep the plain path.
void append_pieces(const MeshFile& mesh, const std::vector<std::size_t>& pieces, CharacterMesh& out,
                   bool surface_maps = false);

// The most frames a resampled clip gets; a longer clip is cut there.
inline constexpr std::uint32_t kMaxClipFrames = 4096;
// The most resampled poses (bone x frame) one model's clips get together; a clip that would pass it is left out.
// Many clips of many bones at kMaxClipFrames each would otherwise take gigabytes.
inline constexpr std::size_t kMaxModelPoses = std::size_t{8} << 20;
// Builds a model: skeleton with inverse bind matrices, the default pieces
// (select_pieces(.., 0, 0)), and clips resampled to fixed frames (at most
// kMaxClipFrames each, and max_poses bone poses in all: a clip past that is left out).
CharacterModel build_model(const MeshFile& mesh, std::string_view code,
                           const std::vector<std::pair<std::string, AnimationFile>>& animations,
                           std::size_t max_poses = kMaxModelPoses);

} // namespace eqg_character

class EqgCharacterStore {
public:
    explicit EqgCharacterStore(GameDir game);

    bool has_model(std::string_view code);
    // The codes OnDemandResources.txt lists a mesh for, sorted.
    std::vector<std::string> codes() const;
    // Loads and caches; nullptr when there is no EQG model for the code.
    std::shared_ptr<const CharacterModel> model(std::string_view code);
    // Files of the EQG archives opened so far (textures of loaded models).
    // A model texture "c_<code>_..." opens <code>.eqg first and is read from
    // it when it has one (texture_owner).
    std::optional<std::vector<std::byte>> read_file(std::string_view name);
    bool has_file(std::string_view name);
    // The parsed mesh file of a loaded model, or nullptr.
    std::shared_ptr<const eqg_character::MeshFile> mesh_file(std::string_view code) const;
    // An attached part's mesh: "<name>.mod" in an EQG archive opened so far
    // (a new model's own parts, "DKM_HAIR_03"), else an OnDemandResources
    // EQGM entry ("IT14162", hero's forge pieces), else a live client's
    // layered part (layered_part). Cached; nullptr if none.
    std::shared_ptr<const eqg_character::MeshFile> part_mesh(std::string_view name);
    // Armor aliases (armor_alias) take precedence over a same-numbered
    // generic resource (which may be a held item or furniture). An existing
    // base with an unusable layer can retain a compatible direct appearance,
    // without enabling modern surface effects. A name with no usable alias
    // resolves as part_mesh does, with surface maps on for a mesh of its own
    // and off where the row's base is missing. Cached; empty if none.
    eqg_character::ArmorPart armor_part_mesh(std::string_view name);
    // Whether `name` has a layered row (layered_part) whose base mesh exists:
    // only then do the armor alias rules replace the piece's plain lookup
    // (part_mesh, or a WLD actor of the same name on a Luclin model).
    bool armor_alias(std::string_view name);
    // How many layered rows part_mesh follows to a base mesh before giving up.
    static constexpr std::uint32_t kMaxLayerDepth = 8;
    // The live client ships each colour of a hero's forge piece as a base
    // mesh plus a texture layer rather than its own .mod (RoF2's IT27104 is
    // IT19814 with GENERIC_CHAIN_BLUE_V1's textures): Resources/Layers/*.txt
    // rows "IT27104^IT19814^GENERIC_CHAIN_BLUE_V1". The base and layer of
    // `name`, from the files sorted by name, the first row winning.
    std::optional<std::pair<std::string, std::string>> layered_part(std::string_view name);
    // A material layer by name ("C_GUS_S01_M01"): from the .lay members of
    // the EQG archives opened so far, else an OnDemandResources EQGL entry.
    // The first one loaded wins (the resource table's Add). nullptr if none.
    const eqg_character::Layer* layer(std::string_view name);

private:
    friend class CharacterLibrary; // cache diagnostics include models retained by the persistent EQG store
    struct Source {
        std::string archive; // lower case, e.g. "hnm.eqg"
        std::string member;  // lower case
    };
    std::shared_ptr<const PfsArchive> archive(const std::string& name);
    std::optional<Source> mesh_source(const std::string& code);
    void open_texture_owner(std::string_view name);
    std::shared_ptr<const eqg_character::MeshFile> part_mesh(std::string_view name, std::uint32_t depth);
    std::vector<std::pair<std::string, Source>> animation_sources(const std::string& code); // clip name, source

    GameDir game_;
    std::unordered_map<std::string, Source> meshes_;                   // "HNM" -> hnm.eqg / hnm.mds
    std::unordered_map<std::string, std::vector<std::pair<std::string, Source>>> animations_; // "DKM" -> ...
    std::unordered_map<std::string, std::shared_ptr<const PfsArchive>> archives_;
    std::unordered_map<std::string, std::string> files_; // lower-case file -> archive
    std::unordered_map<std::string, std::shared_ptr<const CharacterModel>> models_;
    std::unordered_map<std::string, std::shared_ptr<const eqg_character::MeshFile>> mesh_files_;
    std::unordered_map<std::string, std::shared_ptr<const eqg_character::MeshFile>> parts_;
    std::unordered_map<std::string, eqg_character::ArmorPart> armor_parts_;
    void load_layers(const Source& lay);
    std::unordered_map<std::string, Source> layer_sources_;       // "C_GBN_S01_M01" -> gbn.eqg / gbn.lay
    std::unordered_map<std::string, eqg_character::Layer> layers_; // upper-case name -> layer
    std::vector<std::string> loaded_lays_;                         // "gus.eqg/gus.lay"
    std::optional<std::unordered_map<std::string, std::pair<std::string, std::string>>> layered_parts_; // read once
};

} // namespace hailstone::assets
