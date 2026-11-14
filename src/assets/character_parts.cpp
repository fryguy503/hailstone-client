// Luclin-style models (the SoL player models and the player Froglok) and new
// (EQG) models of the classic races (Drakkin), dressed by the original's
// ActorClient functions (game/actor_look.h): body materials by SwapBodyMaterial, hair, beard,
// Drakkin tattoo and facial attachment by ApplyFacialFeatures, armour IT
// parts, helms and hero's forge pieces by ApplyArmorTexture /
// UpdateNewArmorSlot. DressEnv stands for the graphics DLL's model: it
// records what each dag point shows and its SetTint colour, and the parts
// are then built into the mesh.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "assets/character.h"
#include "assets/eqg_character.h"
#include "core/log.h"
#include "game/actor_look.h"
#include "game/appearance.h"
#include "game/appearance_data.h"
#include "game/race_models.h"

namespace hailstone::assets {

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

// "CH" -> chest ... ; -1 for other parts.
int part_slot(std::string_view part) {
    static constexpr std::pair<std::string_view, int> kParts[] = {
        {"HE", game::kSlotHead}, {"CH", game::kSlotChest}, {"UA", game::kSlotArms}, {"FA", game::kSlotWrists},
        {"HN", game::kSlotHands}, {"LG", game::kSlotLegs},  {"FT", game::kSlotFeet},
    };
    for (auto [p, s] : kParts)
        if (p == part) return s;
    return -1;
}

// The material's body slot ("HUMLG0201_MDF" -> legs), or -1.
int material_slot(std::string_view code, std::string_view name) {
    std::string n = upper(name);
    if (!n.ends_with("_MDF") || n.size() != 13 || !n.starts_with(code) || !all_digits(n.substr(5, 4))) return -1;
    return part_slot(n.substr(3, 2));
}

// SetTint (palette SetTint1 @0x10016e90): RGB 0 is untinted; the shader's
// Tint alpha is not used here, so the stored colour is opaque.
std::uint32_t render_tint(std::uint32_t color) { return (color & 0xffffffu) == 0 ? 0u : (color | 0xff000000u); }

// The dag points. Luclin-style models: CDisplay::UpdateDAGPointers @0x40b7f0
// ("<code><name>_DAG"; 0x12 / 0x13 are "<code>FIHANDL/R_TRACK"). New models:
// CDisplay::sub_0040b580 @0x40b580's bone slots (bone names as written).
struct DagPoint {
    std::int32_t dag;
    const char* luclin;
    const char* eqg;
};
constexpr DagPoint kDagPoints[] = {
    {0, "HEHEAD", "HEAD_NAME"},          {1, "HEAD_POINT", "CHEST_CHEST02"}, {2, nullptr, "HEAD_GUILD"},
    {3, "R_POINT", "ARMR_WEAP"},         {4, "L_POINT", "ARML_WEAP"},        {5, "SHIELD_POINT", "ARML_SHLD"},
    {7, "TUNIC_POINT", "ROOT_BODY"},     {8, "HAIR_POINT", "CHEST_CHEST03"}, {9, "BEARD_POINT", "NECK_NECK"},
    {0xa, nullptr, "HEAD_EBROWS"},       {0xb, "CHEST_POINT", "ROOT_BODY"},  {0xc, "GAUNTL_POINT", "ARML_CLAV"},
    {0xd, "GAUNTR_POINT", "ARMR_CLAV"},  {0xe, "PELVIS_POINT", "CHEST03_MOUNT01"}, {0x10, nullptr, "HEAD_CAM01"},
    {0x11, nullptr, "CHEST_CHEST01"},    {0x12, "FIHANDL", "ARML_BCEP"},     {0x13, "FIHANDR", "ARMR_BCEP"},
    {0x14, nullptr, "PELV"},             {0x15, "LEGL_POINT", "LEGL_THGH"},  {0x16, "LEGR_POINT", "LEGR_THGH"},
    {0x17, nullptr, "CHEST03_TORCH01"},  {0x18, nullptr, "HEAD_HEAD"},       {0x19, nullptr, "ROOT_BONE"},
    {0x1a, "SHOULDL_POINT", nullptr},    {0x1b, "SHOULDR_POINT", nullptr},
};

const DagPoint* dag_point(std::int32_t dag) {
    for (const auto& d : kDagPoints)
        if (d.dag == dag) return &d;
    return nullptr;
}

} // namespace

class DressEnv final : public game::ActorEnv {
public:
    struct Attached {
        std::string name;
        std::uint32_t tint = 0;
        std::optional<std::array<float, 16>> adjust;
        std::string overlay;
        std::map<std::int32_t, std::string> materials; // part +0x108
        bool hero_forge = false;
    };

    DressEnv(const CharacterLibrary& lib, const CharacterModel& model, CharacterMesh& mesh, bool eqg)
        : lib_(lib), model_(model), mesh_(mesh), data_(lib.appearance_data()), eqg_(eqg) {}

    std::map<std::int32_t, Attached> parts;
    std::set<std::pair<std::int32_t, std::int32_t>> hidden; // (slot, bank)

    int bone_of(std::int32_t dag) const {
        const DagPoint* d = dag_point(dag);
        if (!d) return -1;
        if (eqg_) {
            if (!d->eqg) return -1;
            for (std::size_t i = 0; i < model_.bone_names.size(); ++i)
                if (upper(model_.bone_names[i]) == d->eqg) return static_cast<int>(i);
            return -1;
        }
        return d->luclin ? model_.find_bone_prefix(d->luclin) : -1;
    }

    bool new_model() override { return eqg_; }
    bool has_dag(std::int32_t dag) override { return bone_of(dag) >= 0; }
    bool has_part(std::int32_t dag) override { return parts.contains(dag); }
    std::optional<std::string> part_object(std::int32_t part) override {
        const DagPoint* d = dag_point(part);
        if (!eqg_ || !d || !d->eqg || bone_of(part) < 0) return std::nullopt;
        return std::string(d->eqg);
    }
    bool mesh_hidden(std::int32_t slot) override {
        return hidden.contains({slot, 0}) || hidden.contains({slot, 1});
    }
    bool attach(std::int32_t dag, const std::string& name, const game::NewArmorAdjustment* adjustment) override {
        return attach_part(dag, name, adjustment, false);
    }
    bool attach_armor(std::int32_t dag, const std::string& name, const game::NewArmorAdjustment* adjustment) override {
        return attach_part(dag, name, adjustment, true);
    }
    bool attach_part(std::int32_t dag, const std::string& name, const game::NewArmorAdjustment* adjustment,
                     bool hero_forge) {
        if (!has_dag(dag)) return false;
        parts.erase(dag);
        if (name.empty() || name == "0" || !exists(name, hero_forge)) return false;
        Attached a;
        a.name = upper(name);
        a.hero_forge = hero_forge;
        if (adjustment) a.adjust = adjustment->matrix;
        parts.emplace(dag, std::move(a));
        return true;
    }
    void tint_part(std::int32_t dag, std::uint32_t color) override {
        if (auto it = parts.find(dag); it != parts.end()) it->second.tint = color;
    }
    void overlay_part(std::int32_t dag, const std::string& name) override {
        if (auto it = parts.find(dag); it != parts.end()) it->second.overlay = name;
    }
    bool replace_material(const std::string& from, const std::string& to, std::uint32_t color) override {
        // CHierarchicalActor::ReplaceMaterial (actor +0xe4): the new material
        // must be loaded; the first palette entry named `from` takes it and
        // the colour (tint1).
        auto layers = lib_.luclin_layers(to);
        if (layers.empty()) return false;
        for (std::size_t mi = 0; mi < mesh_.materials.size(); ++mi) {
            Material& m = mesh_.materials[mi];
            if (upper(m.name) != from) continue;
            m.name = to;
            m.frames = {layers[0]};
            m.tint_layer = layers.size() > 1 ? layers[1] : std::string();
            mesh_.tints[mi] = render_tint(color);
            return true;
        }
        return false;
    }
    bool replace_material_loose(const std::string& from, const std::string& to, std::uint32_t color) override {
        // ReplaceMaterial's mode 1: names compared by characters 0..4, 7 and 8.
        auto layers = lib_.luclin_layers(to);
        if (layers.empty() || from.size() < 9) return false;
        for (std::size_t mi = 0; mi < mesh_.materials.size(); ++mi) {
            Material& m = mesh_.materials[mi];
            std::string n = upper(m.name);
            if (n.size() < 9 || n.compare(0, 5, from, 0, 5) != 0 || n[7] != from[7] || n[8] != from[8]) continue;
            m.name = to;
            m.frames = {layers[0]};
            m.tint_layer = layers.size() > 1 ? layers[1] : std::string();
            mesh_.tints[mi] = render_tint(color);
            return true;
        }
        return false;
    }
    void find_eye_materials(std::int32_t& left, std::int32_t& right, bool halfling_male, bool froglok) override {
        // The palette is the mesh's material list as the model file has it (before any swap).
        for (std::size_t mi = 0; mi < mesh_.materials.size(); ++mi) {
            const std::string n = upper(mesh_.materials[mi].name);
            if (n.size() < 4) continue;
            const std::string_view rest = std::string_view(n).substr(3);
            const auto index = static_cast<std::int32_t>(mi);
            if (rest.starts_with("L_EYE")) left = index;
            else if (rest.starts_with("R_EYE")) right = index;
            else if (halfling_male && rest.starts_with("R_01")) right = index;
            else if (froglok && std::string_view(n).substr(4).starts_with("EYE")) left = right = index;
        }
    }
    bool set_eye_texture(std::uint32_t number, std::int32_t index) override {
        if (index < 0 || static_cast<std::size_t>(index) >= mesh_.materials.size()) return false;
        char name[32];
        std::snprintf(name, sizeof name, "CHR_EYE%03u_MDF", number);
        auto layers = lib_.luclin_layers(name);
        if (layers.empty()) return false;
        Material& m = mesh_.materials[static_cast<std::size_t>(index)];
        m.name = name;
        m.frames = {layers[0]};
        m.tint_layer = layers.size() > 1 ? layers[1] : std::string();
        return true;
    }
    void show_part(std::int32_t slot, std::uint8_t hide, std::int32_t bank) override {
        if (hide) hidden.insert({slot, bank});
        else hidden.erase({slot, bank});
    }
    void robe_tint(std::int32_t, std::uint32_t) override {}
    void robe_off(const std::string&) override {}
    void robe_mesh(std::int32_t, const std::string&) override {}
    std::optional<game::NewArmorTag> find_new_armor_tag(std::uint32_t id, const std::string& code) override {
        return data_ ? data_->new_armor.find_tag(id, code) : std::nullopt;
    }
    const game::NewArmorAdjustment* find_adjustment(std::int32_t part, std::int32_t variant,
                                                    const std::string& code) override {
        return data_ ? data_->new_armor.find_adjustment(part, variant, code) : nullptr;
    }
    std::uint32_t customization_hair_color(const game::ActorLook& look, std::uint8_t index) override {
        return data_ ? data_->customization.hair_color(look.race, look.heritage, look.gender, index) : 0u;
    }
    std::uint32_t customization_base_color(const game::ActorLook& look) override {
        return data_ ? data_->customization.base_color(look.race, look.heritage, look.gender) : 0u;
    }
    game::CustomizationCounts customization_counts(const game::ActorLook& look) override {
        game::CustomizationCounts c;
        if (const game::CustomizationEntry* e = data_ ? data_->customization.find(look.race, look.heritage, look.gender) : nullptr) {
            c.faces = e->faces;
            c.hair_styles = e->hair_styles;
            c.hair_colors = static_cast<std::int32_t>(e->colors.size());
            c.eyes = e->eyes;
            c.beards = e->beards;
            c.tattoos = e->tattoos;
            c.facial_attachments = e->facial_attachments;
        }
        return c;
    }
    bool set_material(std::int32_t /*index*/, const std::string& name) override {
        // New models: the palette entry `index` takes the texture set `name`
        // ("C_DKM_HEAD_S01_M01" -> c_dkm_head_s01_m01_c.dds). The DLL's
        // palette order is not the mesh file's, so the entry is found by its
        // texture family ("c_dkm_head_s").
        if (!eqg_) return false;
        std::string texture = lower(name) + "_c.dds";
        auto s = lower(name).rfind("_s");
        if (s == std::string::npos || !lib_.has_file(texture)) return false;
        std::string family = lower(name).substr(0, s + 2);
        bool any = false;
        for (auto& m : mesh_.materials)
            if (!m.frames.empty() && lower(m.frames.front()).starts_with(family)) {
                m.frames = {texture};
                any = true;
            }
        return any;
    }
    bool set_part_material(std::int32_t dag, std::int32_t index, const std::string& name) override {
        auto it = parts.find(dag);
        if (it == parts.end()) return false;
        it->second.materials[index] = name;
        return true;
    }

    // Builds the attached parts and hides the body meshes ApplyArmorTexture hid.
    void build() {
        if (!eqg_ && !hidden.empty()) {
            std::erase_if(mesh_.parts, [&](const MeshPart& p) {
                int slot = material_slot(model_.code, mesh_.materials[p.material].name);
                return slot >= 0 && mesh_hidden(slot);
            });
        }
        for (const auto& [dag, a] : parts) {
            int anchor = bone_of(dag);
            if (anchor < 0) continue;
            if (!(eqg_ ? attach_eqg(a, anchor) : attach_luclin(a, anchor)))
                log::debug("{}: part {} on dag {:#x} not built", model_.code, a.name, dag);
        }
    }

private:
    const CharacterLibrary& lib_;
    const CharacterModel& model_;
    CharacterMesh& mesh_;
    const game::AppearanceData* data_;
    bool eqg_;

    bool wld_part(const std::string& name) const {
        return lib_.luclin_parts().contains(name) || lib_.actors().contains(name);
    }
    bool exists(const std::string& name, bool hero_forge) const {
        std::string n = upper(name);
        if (hero_forge && lib_.eqg_ && lib_.eqg_->armor_alias(n)) return static_cast<bool>(lib_.eqg_->armor_part_mesh(n));
        if (!eqg_ && wld_part(n)) return true;
        return static_cast<bool>(eqg_part(n, hero_forge));
    }
    // An EQG part's mesh: Hero's Forge armor by its alias rules, else the
    // plain part with no surface maps.
    eqg_character::ArmorPart eqg_part(const std::string& name, bool hero_forge) const {
        if (!lib_.eqg_) return {};
        if (hero_forge) return lib_.eqg_->armor_part_mesh(name);
        return {lib_.eqg_->part_mesh(name), false};
    }

    void append(CharacterMesh& piece, std::uint32_t tint) {
        auto vbase = static_cast<std::uint32_t>(mesh_.vertices.size());
        auto mbase = static_cast<std::uint32_t>(mesh_.materials.size());
        const auto body_bones = model_.bone_names.size();
        const auto prior_bindings = mesh_.skin_bindings.size();
        if (body_bones + prior_bindings + piece.skin_bindings.size() > 0x10000u)
            throw std::runtime_error("too many attachment bones");
        for (auto vertex : piece.vertices) {
            if (!piece.skin_bindings.empty())
                for (auto& bone : vertex.bones)
                    if (bone >= body_bones) bone = static_cast<std::uint16_t>(bone + prior_bindings);
            mesh_.vertices.push_back(vertex);
        }
        mesh_.skin_bindings.insert(mesh_.skin_bindings.end(), piece.skin_bindings.begin(), piece.skin_bindings.end());
        for (const auto& m : piece.materials) {
            mesh_.materials.push_back(m);
            mesh_.tints.push_back(render_tint(tint));
        }
        for (auto p : piece.parts) {
            MeshPart q;
            q.first_index = static_cast<std::uint32_t>(mesh_.indices.size());
            q.index_count = p.index_count;
            q.material = mbase + p.material;
            for (std::uint32_t k = 0; k < p.index_count; ++k) mesh_.indices.push_back(vbase + piece.indices[p.first_index + k]);
            mesh_.parts.push_back(q);
        }
    }

    // A WLD part (lgequip, frogequip): skinned parts follow the body bones of
    // the same names, static ones the dag point; or an EQG piece held rigidly
    // on the point. The NewArmorAdjData matrix applies in the point's space.
    bool attach_luclin(const Attached& a, int anchor) {
        const glm::mat4 adjust = a.adjust ? glm::make_mat4(a.adjust->data()) : glm::mat4(1.0f);
        CharacterMesh piece;
        if (wld_part(a.name) && !(a.hero_forge && lib_.eqg_ && lib_.eqg_->armor_alias(a.name))) {
            const auto* loc = lib_.luclin_parts().find(a.name);
            if (!loc) loc = lib_.actors().find(a.name);
            std::vector<glm::mat4> body;
            model_.sample(model_.clips.at(""), 0.0f, false, body);
            try {
                CharacterModel pm = lib_.build_actor(a.name, *loc->first, *loc->fragment());
                std::vector<glm::mat4> local;
                pm.sample(pm.clips.at(""), 0.0f, false, local);
                std::vector<glm::mat4> correction(pm.bone_names.size());
                std::vector<std::uint16_t> remap(pm.bone_names.size(), static_cast<std::uint16_t>(anchor));
                for (std::size_t b = 0; b < pm.bone_names.size(); ++b) {
                    std::string name = upper(pm.bone_names[b]);
                    bool shared = false;
                    if (name.starts_with(a.name)) {
                        std::string target = model_.code + name.substr(a.name.size());
                        for (std::size_t i = 0; i < model_.bone_names.size(); ++i)
                            if (upper(model_.bone_names[i]) == target) {
                                remap[b] = static_cast<std::uint16_t>(i);
                                shared = true;
                            }
                    }
                    // CHierarchicalActor::AttachToHostBones @0x10043e10: a part bone whose name (past the "IT<n>"
                    // prefix) the body has takes the body bone's matrix itself (0x1003d720 shares the host bone's
                    // matrix pointer), so its vertices, stored in bone space, sit on the body bone as they are. The
                    // part's own rest pose does not move them: the Luclin robes' skeletons hold their sleeve bones
                    // (FOROBEARM*) away from where the body's rest pose has them. Other bones keep the part's own
                    // pose under the dag point.
                    correction[b] = shared ? glm::mat4(1.0f)
                                           : glm::inverse(body[remap[b]]) * body[static_cast<std::size_t>(anchor)] * adjust * local[b];
                }
                piece = pm.default_mesh();
                for (auto& v : piece.vertices) {
                    std::size_t b = v.bones[0];
                    v.position = glm::vec3(correction[b] * glm::vec4(v.position, 1.0f));
                    glm::vec3 n = glm::mat3(correction[b]) * v.normal;
                    v.normal = glm::dot(n, n) > 1e-12f ? glm::normalize(n) : v.normal;
                    v.bones[0] = remap[b];
                }
            } catch (const std::exception&) {
                const wld::WldFile& w = *loc->first;
                const auto& actor = loc->fragment()->as<wld::ActorDef>();
                piece = {};
                try {
                    for (wld::Ref s : actor.sprites) {
                        const auto* ref = w.get<wld::MeshRef>(s);
                        const wld::Fragment* mf = ref ? w.resolve(ref->mesh) : nullptr;
                        if (mf && mf->holds<wld::MeshDef>()) detail::append_wld_mesh(w, *mf, 1, 0, piece);
                    }
                } catch (const std::exception& e) {
                    log::debug("Luclin part {}: {}", a.name, e.what());
                    return false;
                }
                for (auto& v : piece.vertices) {
                    v.position = glm::vec3(adjust * glm::vec4(v.position, 1.0f));
                    v.normal = glm::normalize(glm::mat3(adjust) * v.normal);
                    v.bones[0] = static_cast<std::uint16_t>(anchor);
                }
            }
        } else {
            // An EQG piece (hero's forge, genericplate.eqg): its bones carry
            // the Luclin skeleton's names ("CHChest" -> "<code>CHCHEST_TRACK");
            // each vertex moves from the piece's bind space into its bone's
            // (or the nearest named ancestor's) space and follows that body
            // bone; unnamed ones follow the dag point.
            const auto part = eqg_part(a.name, a.hero_forge);
            if (!part) return false;
            const auto& file = part.mesh;
            std::vector<std::size_t> all(file->pieces.size());
            for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
            eqg_character::append_pieces(*file, all, piece, part.surface_maps);
            if (!eqg_character::bind_part_to_wld(*file, model_.code, model_.bone_names, anchor, adjust, piece)) return false;
            // EQG triangles wind the other way round from WLD ones (the actor renderer turns the front face for
            // EQG models), so on a WLD body each is reversed, or only its inside faces would be drawn.
            for (std::size_t i = 0; i + 2 < piece.indices.size(); i += 3) std::swap(piece.indices[i + 1], piece.indices[i + 2]);
        }
        if (piece.indices.empty()) return false;
        append(piece, a.tint);
        return true;
    }

    // A new model's part ("<code>_HAIR_03", "<code>_01_00_ROOT_BODY",
    // "IT14162"): its bones follow the body's of the same names (bind space
    // as the body's); its material textures by part +0x108 and the overlay.
    bool attach_eqg(const Attached& a, int anchor) {
        const auto part = eqg_part(a.name, a.hero_forge);
        if (!part) return false;
        eqg_character::MeshFile f = *part.mesh;
        for (const auto& [index, name] : a.materials) {
            std::string texture = lower(name) + "_c.dds";
            if (index >= 0 && static_cast<std::size_t>(index) < f.materials.size() && lib_.has_file(texture))
                f.materials[static_cast<std::size_t>(index)].diffuse = texture;
        }
        if (!a.overlay.empty()) {
            // "A_DKM_10_00_S03": the textures "a_dkm_10_00_sNN_..." take S03.
            std::string o = lower(a.overlay);
            auto s = o.rfind("_s");
            if (s != std::string::npos)
                for (auto& m : f.materials) {
                    std::string d = lower(m.diffuse);
                    if (d.size() > s + 4 && d.compare(0, s + 2, o, 0, s + 2) == 0) {
                        std::string c = o + d.substr(s + 4);
                        if (lib_.has_file(c)) m.diffuse = c;
                    }
                }
        }
        std::vector<std::uint16_t> remap(f.bones.size(), static_cast<std::uint16_t>(anchor));
        for (std::size_t b = 0; b < f.bones.size(); ++b)
            for (std::size_t i = 0; i < model_.bone_names.size(); ++i)
                if (upper(model_.bone_names[i]) == upper(f.bones[b].name)) {
                    remap[b] = static_cast<std::uint16_t>(i);
                    break;
                }
        std::vector<std::size_t> all(f.pieces.size());
        for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
        CharacterMesh piece;
        eqg_character::append_pieces(f, all, piece, part.surface_maps);
        for (auto& v : piece.vertices)
            for (std::size_t k = 0; k < 4; ++k) {
                std::size_t b = v.bones[k];
                v.bones[k] = b < remap.size() ? remap[b] : static_cast<std::uint16_t>(anchor);
            }
        if (piece.indices.empty()) return false;
        append(piece, a.tint);
        return true;
    }
};

namespace detail {

game::ActorLook look_of(const CharacterModel& model, const game::Appearance& a, const game::AppearanceData* data) {
    game::ActorLook look;
    if (const game::RaceModel* rm = game::find_model_code(model.code)) {
        look.race = rm->race;
        look.gender = rm->gender == 1 ? 1 : 0;
    }
    look.code = model.code;
    look.face = a.face;
    look.hair_color = a.hair_color;
    look.beard_color = a.beard_color;
    look.eye_color = a.eye_color1;
    look.eye_color2 = a.eye_color2;
    look.hair_style = a.hair_style;
    look.beard = a.beard;
    look.texture_type = a.texture;
    look.head_type = a.helm;
    // The record goes over word for word, as RefreshEquipSlot @00594e50 stores the wire's ArmorProperties at
    // ActorClient +0x8c (PlayerClient_SetActorEquipmentSlot @0042e6f0): EQEmu's Unknown1 is the variation and
    // its EliteMaterial the +8 material UpdateNewArmorSlot overlays (A_<code>_<a>_<b>_S<c>).
    for (std::size_t i = 0; i < 9; ++i) {
        look.tint[i] = a.tint[i];
        look.armor[i].material = static_cast<std::int32_t>(a.material[i]);
        look.armor[i].variation = static_cast<std::int32_t>(a.material_variation[i]);
        look.armor[i].material2 = static_cast<std::int32_t>(a.elite_material[i]);
        look.armor[i].new_armor_id = a.hero_forge[i];
        look.armor[i].new_armor_type = a.new_armor_type[i];
    }
    look.show_helm = a.show_helm;
    look.heritage = static_cast<std::int32_t>(a.drakkin_heritage);
    look.tattoo = static_cast<std::int32_t>(a.drakkin_tattoo);
    look.details = static_cast<std::int32_t>(a.drakkin_details);
    look.new_armor_filtered = game::appearance_new_armor_filtered(a, data ? data->new_armor_filter : 0u);
    return look;
}

} // namespace detail

void CharacterLibrary::dress_luclin(const CharacterModel& model, const game::Appearance& a, CharacterMesh& mesh) const {
    // The palette starts as the model's own materials: <code><part><material>
    // <piece>_MDF, each a skin layer and an armour layer (SkinMeshLuclin's two
    // textures). The head's are the face 0 ones (<code>HE0001_MDF ..) until
    // SwapFace below replaces them.
    const std::string& code = model.code;
    for (std::size_t mi = 0; mi < mesh.materials.size(); ++mi) {
        Material& mat = mesh.materials[mi];
        std::string name = upper(mat.name);
        int slot = material_slot(code, name);
        if (slot < 0) continue;
        const std::string& want = name;
        auto layers = luclin_layers(want);
        if (layers.empty()) layers = luclin_layers(name);
        if (layers.empty()) continue;
        mat.name = want;
        mat.frames = {layers[0]};
        mat.tint_layer = layers.size() > 1 ? layers[1] : std::string();
        mesh.tints[mi] = 0;
    }
    DressEnv env(*this, model, mesh, false);
    game::ActorLook look = detail::look_of(model, a, appearance_data());
    look.luclin_model = true;
    game::find_eye_materials(look, env); // UpdateDAGPointers, when the actor is made
    // ActorClient::SwapAllBodyMaterials @0x40bf80: every body slot from
    // material 0, untinted with a hero's forge piece. Under a robe the sleeves
    // are the chest's (game::luclin_armor_slot).
    if (a.texture == game::kNoTexture)
        for (std::int32_t slot = 1; slot <= 6; ++slot) {
            auto s = static_cast<std::size_t>(game::luclin_armor_slot(a, look.race, slot));
            game::swap_body_material(look, env, slot, 0, static_cast<std::int32_t>(a.material[s]), a.tint[s], 0,
                                     a.class_id, a.hero_forge[s] != 0 ? 1u : 0u);
        }
    // PlayerClient_DressSpawn @0x595c70: SwapFace(face, 0, class) on the fresh
    // model (only with a face when the body texture is fixed); then the
    // features, whose beard colour on the face (male elves, female dwarves)
    // swaps the face again with its tint.
    if (a.texture == game::kNoTexture || a.face != 0) game::swap_face(look, env, a.face, 0, a.class_id);
    game::apply_facial_features(look, env);
    env.build();
}

std::uint32_t CharacterLibrary::try_fheb(const CharacterModel& model, game::Appearance& a, std::uint32_t value,
                                        std::uint32_t type, bool color) const {
    Lock lock(*mutex_);
    wld::WldFile::ReadScope wld_reads;
    CharacterMesh scratch = model.default_mesh();
    DressEnv env(*this, model, scratch, model.eqg);
    game::ActorLook look = detail::look_of(model, a, appearance_data());
    look.luclin_model = model.luclin;
    game::find_eye_materials(look, env);
    std::uint32_t took = color ? game::set_fheb_color(look, env, value, type, 0) : game::set_fheb(look, env, value, type, 0);
    a.face = look.face;
    a.hair_style = look.hair_style;
    a.hair_color = look.hair_color;
    a.beard = look.beard;
    a.beard_color = look.beard_color;
    a.eye_color1 = look.eye_color;
    a.eye_color2 = look.eye_color2;
    return took;
}

void CharacterLibrary::dress_new_model(const CharacterModel& model, const game::Appearance& a, CharacterMesh& mesh) const {
    DressEnv env(*this, model, mesh, true);
    game::ActorLook look = detail::look_of(model, a, appearance_data());
    look.new_style = true;
    game::apply_facial_features(look, env);
    env.build();
}

} // namespace hailstone::assets
