#pragma once

// The original client's ActorClient appearance functions: tints, Luclin
// hair / beard parts and colours, classic body materials, armour textures,
// hero's forge pieces and helms. Each function follows the original's
// behaviour and is checked against the vectors recorded from eqgame.exe
// (tests/appearance_vectors_test.cpp).
//
// The functions work on an ActorLook (the ActorClient fields they read and
// write) and talk to the model through ActorEnv, which stands for the
// graphics DLL's model virtuals and the client's data tables: the renderer
// implements it to build meshes, the tests to record the calls.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace hailstone::game {

// The Luclin hair and beard colours (0x00RRGGBB, table 0xac1a70), indexed by
// the hair / beard colour 0..23.
inline constexpr std::array<std::uint32_t, 24> kLuclinHairColors = {
    0x2e1a0c, 0x432916, 0x4e3123, 0x7f513b, 0x650b06, 0xb93714, 0xd75532, 0x8b721e,
    0xccb361, 0xe1dd6c, 0xfbff81, 0xfdfac9, 0xffffff, 0xdedede, 0x808080, 0x6f8690,
    0x3e585a, 0x293e40, 0x121214, 0xc9e5fd, 0xc9fdfd, 0xe9c9fd, 0xcefdc9, 0x559b48,
};

// The ActorClient's ArmorProperties record of a wear slot (+0x8c + slot * 0x14).
struct ArmorRecord {
    std::int32_t material = 0;
    std::int32_t variation = 0;
    std::int32_t material2 = 0; // the robe overlay / extra texture id
    std::uint32_t new_armor_id = 0; // hero's forge id (0: none)
    std::uint32_t new_armor_type = 0;
};

// The ActorClient fields the appearance functions use (offsets from ActorClient).
struct ActorLook {
    std::int32_t race = 0;       // +0x10
    std::uint8_t gender = 0;     // +0x15 (1 female)
    std::string code;            // NpcManager_GetModelCode(race, gender), "HUM"
    std::uint8_t face = 0;       // +8
    std::uint8_t hair_color = 0; // +9
    std::uint8_t beard_color = 0; // +0xa
    std::uint8_t eye_color = 0;  // +0xb
    std::uint8_t eye_color2 = 0; // +0xc
    std::uint8_t hair_style = 0; // +0xd (0xff: none)
    std::uint8_t beard = 0;      // +0xe (0xff: none)
    std::array<std::uint32_t, 9> tint{}; // +0x58 ArmorColor
    std::array<ArmorRecord, 9> armor{};  // +0x8c; armor[1].material is the robe (+0xa0)
    bool show_helm = true;       // +0x7c
    std::int32_t heritage = 0;   // +0x80 (Drakkin heritage, customization key)
    std::int32_t tattoo = 0;     // +0x84
    std::int32_t details = 0;    // +0x88 (facial attachment)
    std::int32_t face_variant = 0; // +0x1a8
    bool new_armor_filtered = false; // +0x1ac (1: hero's forge not shown)
    std::uint8_t texture_type = 0xff; // +4 TextureType (0xff: per-slot player body)
    std::uint8_t head_type = 0;  // +7 HeadType (SwapFace's head variant for a fixed body texture)
    // +0x140 / +0x144: the model palette entries of the left and right eye, which UpdateDAGPointers @0x40b7f0
    // finds through the actor's +0x150 (ActorEnv::find_eye_materials); -1 for none found.
    std::int32_t eye_left = -1;
    std::int32_t eye_right = -1;
    bool luclin_model = false;   // the animation set's +0x34 (a SoL model)
    bool new_style = false;      // the ActorClient's virtual +8 (new-style model, Drakkin and the HD races)
};

// A NewArmorTagData.txt row (NewArmor_FindTag's record).
struct NewArmorTag {
    std::int32_t it_number = 0;
    std::int32_t it_pair = 0;
    bool ignore_adjustment = false;
    bool show_hair = false;
    bool hide_mesh = false;
};

// A NewArmorAdjData.txt row: the attachment matrix (column-major, the
// bone-space transform applied to the IT model) for (part, variant, code).
struct NewArmorAdjustment {
    std::int32_t part = 0;
    std::int32_t variant = 0;
    std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// The customization counts of the actor's {race, heritage, gender} entry
// (0 without one): CPlayerCustomizationManager GetNum*.
struct CustomizationCounts {
    std::int32_t faces = 0, hair_styles = 0, hair_colors = 0, eyes = 0, beards = 0, tattoos = 0,
                 facial_attachments = 0;
};

// The model and the tables, as the original reaches them.
class ActorEnv {
public:
    virtual ~ActorEnv() = default;
    // Model virtuals (CHierarchicalModel, EQGraphicsDX9.dll).
    // +0xbc: true for the new (EQG / HD) models, which take UpdateNewArmorSlot
    // and the "<code>_HAIR_nn" parts (an old-model check read the other
    // way; the armour functions show which way it goes).
    virtual bool new_model() = 0;
    virtual bool has_dag(std::int32_t dag) = 0;   // +0x1a4
    virtual bool has_part(std::int32_t dag) = 0;  // +0x1a8 returns the dag's part
    // +0x1c4: the part object of a model part and its name, or nullopt.
    virtual std::optional<std::string> part_object(std::int32_t part) = 0;
    virtual bool mesh_hidden(std::int32_t slot) = 0; // +0xec
    // +0x134: shows `name` ("IT<n>", "<code>_HAIR_01", "0" or "" for none) on
    // dag `dag`, with a NewArmorAdjData matrix or none; whether it took.
    virtual bool attach(std::int32_t dag, const std::string& name, const NewArmorAdjustment* adjustment) = 0;
    // Hero's Forge attachments resolve layered armor before direct item models.
    // Other environments keep the same attachment calls (including recorded vectors).
    virtual bool attach_armor(std::int32_t dag, const std::string& name, const NewArmorAdjustment* adjustment) {
        return attach(dag, name, adjustment);
    }
    // The dag's part's SetTint (part virtual +0x114, children 0).
    virtual void tint_part(std::int32_t dag, std::uint32_t color) = 0;
    // The dag's part's overlay texture (part virtual +0x24c).
    virtual void overlay_part(std::int32_t dag, const std::string& name) = 0;
    // +0xe4 ReplaceMaterial(from, to, &color, 0, null); whether it took.
    virtual bool replace_material(const std::string& from, const std::string& to, std::uint32_t color) = 0;
    // +0xe4 with mode != 0: the palette entry matches `from` by characters 0..4, 7 and 8 only (SwapFace's
    // HeadType path for face index 0). A null colour pointer is passed as 0.
    virtual bool replace_material_loose(const std::string& /*from*/, const std::string& /*to*/,
                                        std::uint32_t /*color*/) {
        return false;
    }
    // +0x150 (CHierarchicalActor @0x10040c50): every palette entry whose material name, past its three-letter
    // model code, starts "L_EYE" sets `left` and "R_EYE" `right`; with `halfling_male` one starting "R_01" sets
    // `right`, with `froglok` one whose name from its fifth letter starts "EYE" sets both. Unmatched: unchanged.
    virtual void find_eye_materials(std::int32_t& /*left*/, std::int32_t& /*right*/, bool /*halfling_male*/,
                                    bool /*froglok*/) {}
    // +0x15c (@0x10040d50): palette entry `index` (0 <= index < the palette's count) takes the loaded material
    // "CHR_EYE<number, 3 digits>_MDF"; false when either is missing.
    virtual bool set_eye_texture(std::uint32_t /*number*/, std::int32_t /*index*/) { return false; }
    // +0xe8: hide (1) or show body mesh `slot` of bank `bank`.
    virtual void show_part(std::int32_t slot, std::uint8_t hide, std::int32_t bank) = 0;
    virtual void robe_tint(std::int32_t index, std::uint32_t color) = 0; // +0x13c
    virtual void robe_off(const std::string& code) = 0;                  // +0x140
    virtual void robe_mesh(std::int32_t on, const std::string& code) = 0; // +0x144

    // Tables: NewArmorTagData / NewArmorAdjData lookups, and the
    // customization manager's hair colour (ActorClient::GetHairColorTint).
    virtual std::optional<NewArmorTag> find_new_armor_tag(std::uint32_t id, const std::string& code) = 0;
    virtual const NewArmorAdjustment* find_adjustment(std::int32_t part, std::int32_t variant,
                                                      const std::string& code) = 0;
    virtual std::uint32_t customization_hair_color(const ActorLook& look, std::uint8_t index) = 0;
    virtual std::uint32_t customization_base_color(const ActorLook&) { return 0; } // GetBaseColor
    virtual CustomizationCounts customization_counts(const ActorLook&) { return {}; }
    // Model +0x108: material `index` takes the texture set `name`
    // ("C_DKM_HEAD_S01_M01"); the dag's part's +0x108 likewise.
    virtual bool set_material(std::int32_t /*index*/, const std::string& /*name*/) { return false; }
    virtual bool set_part_material(std::int32_t /*dag*/, std::int32_t /*index*/, const std::string& /*name*/) {
        return false;
    }
    // The seven dwords at 0xde09e8 (material values 17..23 stay when set
    // and the race is not Vah Shir or Froglok): 0 in the original's data.
    virtual bool keep_extended_material(int /*index*/) { return false; }

    // Nested actor functions (overridable so the tests can record them).
    virtual std::uint32_t set_fheb(ActorLook& look, std::uint32_t id, std::uint32_t type, std::uint32_t arg);
    virtual void refresh_hair(ActorLook& look, std::uint8_t show_hair);
};

// ActorClient::GetAllowedHairColorIndexRange @0x40b0c0: the Luclin hair
// colour range of `race` (negative: the actor's own).
struct HairColorRange {
    std::int32_t low = 0;
    std::int32_t high = 0;
};
HairColorRange allowed_hair_color_range(std::int32_t race);

// ActorClient_GetBeardKind @0x40a240: 1 beard model, 2 beard colour through
// the face (male elves, female dwarves), 0 none.
std::uint8_t beard_kind(std::int32_t race, std::uint8_t gender);

// ActorClient_SplitFace @0x40c2a0: face -> head index and variant (nullopt:
// not written). `face` < 0 takes the current face.
struct SplitFace {
    std::uint8_t index = 0;
    std::optional<std::uint8_t> variant;
};
SplitFace split_face(std::int32_t face, std::uint8_t current_face, std::int32_t race, bool new_style);

// ActorClient_NormalizeSlotTextureIds @0x40b4d0: the new-model texture id
// `a` of a slot's material, and the robe overlay number written to `c`.
void normalize_slot_texture_ids(std::int32_t slot, std::int32_t& a, std::int32_t& c);

// ActorClient_ApplyNpcTint @0x40cea0: the npct.ini entry an NPC tint index
// selects (index - 1; ColorTable_GetEntry's signed check against
// `table_count`), or nullopt (index 0 or past the table: untouched).
std::optional<std::int32_t> npc_tint_entry(std::int32_t index, std::int32_t table_count);

// ActorClient::GetRaceSexITOffset @0x40a290.
std::int32_t race_sex_it_offset(std::int32_t race, std::uint8_t gender);

// Races drawn with the classic heads (<= 12, 128, 130, 330, 522), signed.
bool classic_head_race(std::int32_t race);

// ActorClient::AttachHelmIT @0x40aa30: IT<id + race offset> on dag 8 (ids
// 2000 and up: 9); Vah Shir (unless face 7) and Froglok take IT0 and lose
// hair and beard. Whether it took.
bool attach_helm_it(ActorLook& look, ActorEnv& env, std::int32_t id);

// ActorClient::SetFHEB @0x40d770 for the old / Luclin models: the face (type
// 1) through SwapFace and, on a Luclin model, the right eye again; hair (2)
// and beard (4) styles; both eyes (3).
std::uint32_t set_fheb(ActorLook& look, ActorEnv& env, std::uint32_t id, std::uint32_t type, std::uint32_t arg);

// ActorClient::SetFHEB_Color @0x40d930: Luclin hair (type 2..3, others),
// beard (4) and face (1) colours. Returns the original's low byte.
std::uint32_t set_fheb_color(ActorLook& look, ActorEnv& env, std::uint32_t color, std::uint32_t type,
                             std::uint32_t arg);

// ActorClient::sub_0040dac0 @0x40dac0: the hair after a helm change.
void refresh_hair(ActorLook& look, ActorEnv& env, std::uint8_t show_hair);

// ActorClient::sub_00409770 @0x409770: the material values a body swap uses.
void normalize_body_materials(const ActorLook& look, ActorEnv& env, std::int32_t& from, std::int32_t& to,
                              std::int32_t kind);

// ActorClient::SwapBodyMaterial @0x4099f0: classic / Luclin body textures
// "<code><CH|UA|FA|HN|LG|FT><material>01_MDF" (Vah Shir, Froglok: with the
// variant digit) for slots 1..6. Returns the original's byte.
std::uint8_t swap_body_material(ActorLook& look, ActorEnv& env, std::int32_t slot, std::int32_t from,
                                std::int32_t to, std::uint32_t tint, std::int32_t variant, std::int32_t kind,
                                std::uint32_t no_tint);

// ActorClient::UpdateNewArmorSlot @0x40ded0: a new-model slot's texture,
// hero's forge piece and (head) hair.
void update_new_armor_slot(ActorLook& look, ActorEnv& env, std::uint32_t slot);

// ActorClient::ApplyArmorTexture @0x40e450: old / Luclin model armour IT
// parts, hidden meshes, hero's forge and helms for slots `slot`..6 (-1: all).
void apply_armor_texture(ActorLook& look, ActorEnv& env, std::int32_t slot);

// ActorClient::SetFacialFeatureNewModel @0x40c7d0 for the features
// ApplyFacialFeatures uses: 2 hair style (with its colour), 9 tattoo, 10
// facial attachment.
std::uint32_t set_facial_feature_new_model(ActorLook& look, ActorEnv& env, std::int32_t value, std::int32_t type);

// ActorClient::SetTattoo @0x40ae60: "<code>_TATTOO_00" on dag 0x19 and, for
// tattoo > 0, its texture "A_<code>_TATTOO_S<tattoo>_M01".
bool set_tattoo(ActorLook& look, ActorEnv& env, std::int32_t tattoo);

// ActorClient::ApplyFacialFeatures @0x40ecf0: puts the recorded features
// back on the model (new-style classic races through the customization
// lists: face, hair, beard, eyes, tattoo and facial attachment tinted with
// the heritage's BASE_COLOR; Luclin models: hair and beard IT parts and
// their colours, both eyes), then ApplyArmorTexture(-1).
void apply_facial_features(ActorLook& look, ActorEnv& env, std::uint32_t arg = 0xffffffffu);

// The end of CDisplay::UpdateDAGPointers @0x40b7f0: the actor's +0x150 finds
// the eye palette entries (+0x140 left, +0x144 right), a male Halfling's
// "R_01" entry and a Froglok's "?EYE" entries included.
void find_eye_materials(ActorLook& look, ActorEnv& env);

// ActorClient::sub_0040c350 @0x40c350: an eye's material on a Luclin model,
// "CHR_EYE<material + race offset>_MDF" on palette entry +0x144 (`secondary`,
// the right eye; some face / race pairs take material 0xd1 or 0xcf there, the
// eye patches) or +0x140; the material is recorded at +0xc / +0xb. New-style
// classic races go to SetEyeMaterial. Returns the original's 0 / 1.
std::uint32_t set_luclin_eye(ActorLook& look, ActorEnv& env, bool secondary, std::uint32_t material);

// ActorClient::SwapFace @0x40d1a0 for old and Luclin models: replaces the
// head materials "<code>HE<variant 2 digits><index>1_MDF" of face `from` with
// face `to`'s (split_face), stepping the piece digit through the head's
// parts; Iksar, Vah Shir and Froglok redo their body materials with the new
// face index; records the face (+8) and the face index (+0x1a8). New-style
// classic races go to SetFacialFeatureNewModel. Returns the original's byte.
std::uint8_t swap_face(ActorLook& look, ActorEnv& env, std::int32_t to, std::int32_t from, std::int32_t kind);

} // namespace hailstone::game
