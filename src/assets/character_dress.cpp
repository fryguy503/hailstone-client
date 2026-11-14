// Character appearance: armour and face textures, helm and robe meshes,
// tints and held items on a CharacterModel.
//
// Sources: the original's ApplyBodyTexture, RefreshEquipSlot and SetHead
// for which value drives which part; the texture
// naming rules (<code><part><variant><piece>, faces in the piece's tens
// digit, robes 10..16 -> body mesh <code>01 with CLK04..CLK10 textures,
// helm meshes <code>HE<nn>, items IT<n> on the R_POINT / L_POINT /
// SHIELD_POINT bones) follow OpenEQ, except which secondary is a shield
// (CDisplay::IsShield, game::is_shield_model)
// crates/openeq-assets/src/character/appearance.rs (MIT OR Apache-2.0); see
// THIRD_PARTY.md. Format notes: docs/formats/characters.md ("Appearance").

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>

#include "assets/character.h"
#include "assets/eqg_character.h"
#include "assets/texture_overrides.h"
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

std::string two_digits(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02u", v % 100);
    return buf;
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

// SwapBodyMaterial's colour is the tint's RGB with alpha 0xff, and the
// palette's SetTint1 treats RGB 0 as untinted; the alpha byte the server sends does not matter.
bool tint_visible(std::uint32_t tint) { return (tint & 0x00ffffffu) != 0; }

std::string appearance_key(const CharacterModel& m, const game::Appearance& a) {
    std::string k = m.code;
    auto add = [&](std::uint32_t v) {
        k += ':';
        k += std::to_string(v);
    };
    add(a.texture);
    add(a.helm);
    add(a.face);
    add(a.hair_style);
    add(a.beard);
    add(a.show_helm);
    add(a.new_armor_bits);
    for (std::size_t i = 0; i < 9; ++i) {
        add(a.material[i]);
        add(a.material_variation[i]);
        add(a.elite_material[i]);
        add(a.tint[i]);
        add(a.hero_forge[i]);
        add(a.new_armor_type[i]);
    }
    for (std::uint32_t v : {std::uint32_t{a.hair_color}, std::uint32_t{a.beard_color}, std::uint32_t{a.eye_color1},
                            std::uint32_t{a.eye_color2}, a.drakkin_heritage, a.drakkin_tattoo, a.drakkin_details,
                            a.npc_tint, std::uint32_t{a.class_id}})
        add(v);
    return k;
}

} // namespace

bool CharacterLibrary::has_item(std::uint32_t number) const {
    if (number == 0) return false;
    return actors().contains("IT" + std::to_string(number));
}

std::optional<CharacterLibrary::ItemMesh> CharacterLibrary::actor_mesh(const std::string& code) const {
    Lock lock(*mutex_);
    wld::WldFile::ReadScope wld_reads;
    std::optional<ItemMesh> out;
    if (const FragmentLoc* loc = actors().find(code)) {
        try {
            const wld::WldFile& w = *loc->first;
            const auto& actor = loc->fragment()->as<wld::ActorDef>();
            CharacterMesh mesh;
            // Static items: 0x2d mesh references, vertices in item space.
            for (wld::Ref s : actor.sprites) {
                const auto* ref = w.get<wld::MeshRef>(s);
                const wld::Fragment* mf = ref ? w.resolve(ref->mesh) : nullptr;
                if (mf && mf->holds<wld::MeshDef>())
                    detail::append_wld_mesh(w, *mf, 1, 0, mesh);
            }
            if (mesh.indices.empty()) {
                // Skeletal items (bows, some shields): bake the rest pose.
                CharacterModel m = build(code);
                std::vector<glm::mat4> world;
                m.sample(m.clips.at(""), 0.0f, false, world);
                for (auto& v : m.vertices) {
                    const glm::mat4& b = world[v.bones[0]];
                    v.position = glm::vec3(b * glm::vec4(v.position, 1.0f));
                    v.normal = glm::normalize(glm::mat3(b) * v.normal);
                    v.bones[0] = 0;
                }
                mesh = m.default_mesh();
            }
            if (!mesh.indices.empty()) out = ItemMesh{std::move(mesh), false, {}, {}};
        } catch (const std::exception& e) {
            log::debug("actor model {}: {}", code, e.what());
        }
    }
    return out;
}

std::optional<CharacterLibrary::ItemMesh> CharacterLibrary::item_mesh(std::uint32_t number) const {
    Lock lock(*mutex_);
    wld::WldFile::ReadScope wld_reads;
    if (auto it = item_cache_.find(number); it != item_cache_.end()) {
        CharacterLookups::note(it->second.lookups);
        return it->second.value;
    }
    if (shared_)
        if (auto it = shared_->items.find(number); it != shared_->items.end() && unaffected(it->second.lookups)) {
            it->second.used = shared_->generation();
            CharacterLookups::note(it->second.lookups);
            item_cache_.emplace(number, Cached<std::optional<ItemMesh>>{it->second.value, it->second.lookups});
            return it->second.value;
        }
    std::optional<ItemMesh> out;
    CharacterLookups::Names names;
    {
        std::optional<CharacterLookups> rec;
        if (shared_ || CharacterLookups::active()) rec.emplace();
        build_item_mesh(number, out);
        if (rec) names = rec->names();
    }
    if (shared_ && names && unaffected(*names))
        shared_->items.try_emplace(number, SharedCharacterCache::Entry<std::optional<ItemMesh>>{out, names, shared_->generation()});
    item_cache_.emplace(number, Cached<std::optional<ItemMesh>>{out, std::move(names)});
    return out;
}

void CharacterLibrary::build_item_mesh(std::uint32_t number, std::optional<ItemMesh>& out) const {
    std::string code = "IT" + std::to_string(number);
    if (actors().contains(code)) {
        out = actor_mesh(code);
    } else if (eqg_) {
        // An EQG item (OnDemandResources EQGM): its pieces in its own space, held rigidly; its .pts and .prt
        // beside it in the archive part_mesh opened (the EQGM loader reads both, 0x100624e0).
        if (auto file = eqg_->part_mesh(code)) {
            ItemMesh item;
            item.eqg = true;
            std::vector<std::size_t> all(file->pieces.size());
            for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
            eqg_character::append_pieces(*file, all, item.mesh);
            for (auto& v : item.mesh.vertices) {
                v.bones = {0, 0, 0, 0};
                v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
            }
            try {
                if (auto pts = eqg_->read_file(lower(code) + ".pts")) item.points = parse_pts(*pts);
                if (auto prt = eqg_->read_file(lower(code) + ".prt")) item.emitters = parse_prt(*prt);
            } catch (const std::exception& e) {
                log::debug("item model {} points: {}", code, e.what());
            }
            if (!item.mesh.indices.empty()) out = std::move(item);
        }
    }
}

std::shared_ptr<const CharacterMesh> CharacterLibrary::dress(const CharacterModel& model,
                                                             const game::Appearance& a) const {
    Lock lock(*mutex_);
    wld::WldFile::ReadScope wld_reads;
    // A model from its own archive (ModelArchiveStore) is dressed by that archive's library.
    if (!model.eqg && own_ && !actors().contains(model.code))
        if (auto lib = own_->opened(model.code)) return lib->dress(model, a);
    std::string key = appearance_key(model, a);
    if (auto it = dress_cache_.find(key); it != dress_cache_.end()) {
        CharacterLookups::note(it->second.lookups);
        return it->second.value;
    }
    // A model of the shared cache is dressed into it too (keyed by the model object, so never another source's).
    const bool share = shares(&model);
    if (share)
        if (auto it = shared_->dresses.find({&model, key}); it != shared_->dresses.end() && unaffected(it->second.lookups)) {
            it->second.used = shared_->generation();
            CharacterLookups::note(it->second.lookups);
            dress_cache_.emplace(key, Cached<std::shared_ptr<const CharacterMesh>>{it->second.value, it->second.lookups});
            return it->second.value;
        }
    std::optional<CharacterLookups> rec;
    if (share || CharacterLookups::active()) rec.emplace();
    std::shared_ptr<const CharacterMesh> result;
    try {
        CharacterMesh mesh = model.eqg ? dress_eqg(model, a) : dress_wld(model, a);
        // The EQG model's own actor emitters, on its points' bones.
        for (const auto& e : model.emitters) {
            const ModelPoint* p = find_point(model.points, e.point);
            if (!p) continue;
            for (std::size_t b = 0; b < model.bone_names.size(); ++b)
                if (model.bone_names[b] == p->bone) {
                    mesh.emitters.push_back({static_cast<int>(b), point_matrix(*p), e, model.code});
                    break;
                }
        }
        apply_npc_tint(model, a, mesh);
        attach_items(model, a, mesh);
        result = std::make_shared<const CharacterMesh>(std::move(mesh));
        log::debug("dressed {}: {} vertices, {} materials", key, result->vertices.size(), result->materials.size());
    } catch (const std::exception& e) {
        log::debug("dressing {}: {}", key, e.what());
    }
    CharacterLookups::Names names = rec ? rec->names() : nullptr;
    if (share && names && unaffected(*names) && !shared_->dresses.contains({&model, key})) {
        shared_->dresses.emplace(std::pair{&model, key}, SharedCharacterCache::Entry<std::shared_ptr<const CharacterMesh>>{
                                                             result, names, shared_->generation()});
        if (result) shared_->owners[result.get()] = {names, &model};
    }
    dress_cache_.emplace(std::move(key), Cached<std::shared_ptr<const CharacterMesh>>{result, std::move(names)});
    return result;
}

namespace {

// The head (PlayerClient::SwapHead's old-model path, game::head_model): NPC
// bodies take HeadType; PC-model races their head armour, only with
// show_helm. The race and gender are the model code's (0x40a5b0 reads the
// actor's).
// The PC race and gender whose model code this is (0 for other models).
std::pair<std::uint32_t, std::uint8_t> pc_race_of(const CharacterModel& model) {
    std::uint32_t race = 0;
    std::uint8_t gender = 0;
    for (std::uint32_t r : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 128u, 130u})
        for (std::uint8_t g : {std::uint8_t{0}, std::uint8_t{1}})
            if (const auto* rm = game::find_race_model(static_cast<std::uint16_t>(r), g); rm && rm->code == model.code) {
                race = r;
                gender = g;
            }
    return {race, gender};
}

game::HeadModel head_of(const CharacterModel& model, const game::Appearance& a) {
    const auto [race, gender] = pc_race_of(model);
    const bool body_texture = a.texture != game::kNoTexture;
    const std::uint32_t material = body_texture ? a.helm : (a.show_helm ? a.material[game::kSlotHead] : 0u);
    return game::head_model(race, gender, material, a.hero_forge[game::kSlotHead]);
}

} // namespace

CharacterMesh CharacterLibrary::dress_wld(const CharacterModel& model, const game::Appearance& a) const {
    const std::string& code = model.code;
    const FragmentLoc* loc = actors().find(code);
    if (!loc) return model.default_mesh();
    const wld::WldFile& w = *loc->first;
    const auto& actor = loc->fragment()->as<wld::ActorDef>();
    const wld::Skeleton* sk = nullptr;
    for (wld::Ref s : actor.sprites)
        if (const auto* sr = w.get<wld::SkeletonRef>(s))
            if ((sk = w.get<wld::Skeleton>(sr->skeleton))) break;
    if (!sk) return model.default_mesh();
    const std::size_t n = sk->nodes.size();
    const bool body_texture = a.texture != game::kNoTexture;

    // Head and body meshes. SetHead builds "<code>HE%02d"; robes (chest
    // 10..16) replace the body with "<code>01".
    const game::HeadModel head = head_of(model, a);
    std::uint32_t head_variant = head.variant;
    // Whether SetHead's head DAG <code>HE<nn> exists (HE00 is the model's own).
    const bool head_took = head.variant == 0 ||
                           (head.variant < 100 && meshes().contains(code + "HE" + two_digits(head.variant) + "_DMSPRITEDEF"));
    const bool robe = !body_texture && !model.luclin && game::is_robe(a.material[game::kSlotChest]);
    if (model.luclin) head_variant = 0; // Luclin helms are attached parts
    auto alternative = [&](const std::string& name) -> FragmentLoc {
        const FragmentLoc* found = meshes().find(name);
        return found ? *found : FragmentLoc{};
    };
    std::vector<std::pair<wld::Ref, std::int64_t>> sprites;
    for (wld::Ref r : sk->meshes) sprites.emplace_back(r, -1);
    if (sprites.empty())
        for (std::size_t i = 0; i < n; ++i)
            if (sk->nodes[i].sprite) sprites.emplace_back(sk->nodes[i].sprite, static_cast<std::int64_t>(i));
    CharacterMesh out;
    std::unordered_set<const wld::Fragment*> seen;
    for (auto [s, owner] : sprites) {
        const wld::Fragment* sprite = w.resolve(s);
        const auto* ref = sprite ? sprite->get<wld::MeshRef>() : nullptr;
        const wld::Fragment* mf = ref ? w.resolve(ref->mesh) : nullptr;
        if (!mf || !mf->holds<wld::MeshDef>()) continue;
        const wld::WldFile* mw = &w;
        std::string name = upper(mf->name);
        FragmentLoc alt{};
        if (name == code + "HE00_DMSPRITEDEF" && head_variant > 0 && head_variant < 100)
            alt = alternative(code + "HE" + two_digits(head_variant) + "_DMSPRITEDEF");
        else if (name == code + "_DMSPRITEDEF" && robe)
            alt = alternative(code + "01_DMSPRITEDEF");
        if (const auto* alternate_mesh = alt.fragment()) {
            mw = alt.first;
            mf = alternate_mesh;
        }
        if (!seen.insert(mf).second) continue;
        detail::append_wld_mesh(*mw, *mf, n, owner, out);
    }
    if (out.indices.empty()) return model.default_mesh();
    if (model.luclin) {
        dress_luclin(model, a, out);
        return out;
    }

    // Textures: <code><part><variant><piece>.
    for (std::size_t mi = 0; mi < out.materials.size(); ++mi) {
        Material& mat = out.materials[mi];
        int material_slot = -1;
        bool tintable = false;
        for (auto& frame : mat.frames) {
            auto dot = frame.rfind('.');
            if (dot == std::string::npos) continue;
            std::string stem = lower(frame.substr(0, dot));
            std::string ext = frame.substr(dot);
            std::string replacement;
            if (stem.size() == 9 && all_digits(stem.substr(5))) {
                int slot = part_slot(upper(stem.substr(3, 2)));
                if (slot < 0) continue;
                material_slot = slot;
                std::string base = stem;
                if (slot == game::kSlotHead) {
                    // NPC body textures: the actor's SwapNpcMaterials (vtable
                    // +0x164, EQGraphicsDX9 @0x10043630) replaces characters
                    // [5..6] of every <code>??dddd material, heads included,
                    // with TextureType's "%02d", but only when that material
                    // exists; otherwise the head keeps what SwapNPCMaterials'
                    // loop left: up to 4 tries of the same HeadType head DAG
                    // (SwapHead ignores the face), then the head's variation
                    // when the DAG took, else TextureType's (checked against the recorded vectors;
                    // PlayerClient::sub_0058f9c0's texture rule
                    // is read as that variant's material when it exists).
                    if (body_texture) {
                        for (std::uint32_t v : {std::uint32_t{a.texture}, std::uint32_t{a.helm}}) {
                            if (v >= 100 || (v == 0 && v != a.texture)) continue;
                            if (v != a.texture && !head_took) continue;
                            std::string c = stem.substr(0, 5) + two_digits(v) + stem.substr(7);
                            if (has_file(c + ext)) {
                                base = c;
                                break;
                            }
                        }
                    }
                    // The face is the piece's tens digit (HUMHE0011 = face 1).
                    if (a.face > 0 && a.face < 10) {
                        std::string c = base.substr(0, 7) + static_cast<char>('0' + a.face) + base.substr(8);
                        if (has_file(c + ext)) base = c;
                    }
                    if (base != stem) replacement = base;
                    tintable = base.substr(5, 2) != "00"; // exposed skin stays untinted
                } else {
                    std::uint32_t v = game::effective_material(a, slot);
                    if (game::is_robe(v)) v -= 6; // robes use variants 04..10
                    tintable = game::effective_material(a, slot) > 0;
                    if (v > 0 && v < 100) {
                        std::string c = stem.substr(0, 5) + two_digits(v) + stem.substr(7);
                        // SwapBodyMaterial's ReplaceMaterial (EQGraphicsDX9 0x10040580) fails, colour and all, when
                        // the new variant's material is not loaded: a model without that armour variant (SKE has chest
                        // variants 00..04, so no SKECH05) keeps its own texture, untinted.
                        if (has_file(c + ext))
                            replacement = c;
                        else
                            tintable = false;
                    }
                }
            } else if (stem.size() == 7 && stem.starts_with("clk") && all_digits(stem.substr(3))) {
                material_slot = game::kSlotChest;
                std::uint32_t chest = a.material[game::kSlotChest];
                if (game::is_robe(chest)) {
                    std::string c = "clk" + two_digits(chest - 6) + stem.substr(5);
                    if (has_file(c + ext)) replacement = c;
                }
                tintable = true;
            }
            if (!replacement.empty()) frame = upper(replacement) + ext;
        }
        if (material_slot >= 0 && tintable) {
            std::uint32_t t = a.tint[static_cast<std::size_t>(material_slot)];
            if (tint_visible(t)) out.tints[mi] = t | 0xff000000u;
        }
    }
    // SwapHead's head DAG (actor +0x14c, CHierarchicalActor @0x10043250): once the head mesh took, every palette
    // material named HELM* or CHAIN* (a Barbarian's BAMHE0003*, BAMHE0004* and CHAIN01*) takes the head's armour
    // colour (SetTint1: black is untinted). SwapHead does nothing for a PC race without show helm; Vah Shir wear
    // their helms as items. The "keep head" flag 0x40a5b0 gives some female and Erudite helms reaches the DLL
    // only to be passed on to the actor's child (+0x58), so it changes nothing.
    const std::uint32_t race = pc_race_of(model).first;
    const bool head_dag = race != 130 && (race == 0 || body_texture || a.show_helm);
    if (head_dag && head_took) {
        const std::uint32_t t = a.tint[game::kSlotHead];
        for (std::size_t mi = 0; mi < out.materials.size(); ++mi) {
            const std::string upper_name = upper(out.materials[mi].name);
            const bool helm = race == 2 ? upper_name.starts_with("BAMHE0003") || upper_name.starts_with("BAMHE0004") ||
                                              upper_name.starts_with("CHAIN01")
                                        : upper_name.starts_with("HELM") || upper_name.starts_with("CHAIN");
            if (helm) out.tints[mi] = tint_visible(t) ? (t | 0xff000000u) : 0u;
        }
    }
    return out;
}

std::vector<std::string> CharacterLibrary::luclin_layers(std::string_view material) const {
    Lock lock(*mutex_);
    wld::WldFile::ReadScope wld_reads;
    std::vector<std::string> out;
    const FragmentLoc* loc = luclin_materials().find(upper(material));
    if (!loc) return out;
    const wld::WldFile& w = *loc->first;
    const auto& def = loc->fragment()->as<wld::MaterialDef>();
    const auto* sprite = w.get<wld::BitmapInfoRef>(def.bitmap);
    const auto* info = sprite ? w.get<wld::BitmapInfo>(sprite->info) : nullptr;
    const auto* bmp = info && !info->frames.empty() ? w.get<wld::BitmapName>(info->frames.front()) : nullptr;
    if (!bmp) return out;
    for (std::string f : bmp->files) {
        // Layer names may carry a "_LAYER" suffix (OpenEQ luclin.rs).
        std::string l = lower(f);
        if (l.ends_with("_layer")) f = f.substr(0, f.size() - 6);
        out.push_back(f);
    }
    return out;
}

std::optional<Image> CharacterLibrary::texture(std::string_view name, bool masked, std::string* error) const {
    if (name.starts_with("layers:")) return layered_texture(name, error);
    auto data = read_file(name);
    std::string why;
    // Character BMPs store their rows bottom-up relative to the model UVs, as the WLD zones' do; DDS files (even when
    // named .bmp) already match (OpenEQ docs/CHARACTER_RENDERING.md).
    auto image = decode_game_texture(name, data, {.palette_index0_transparent = masked, .wld_bmp_rows = true}, &why);
    if (!image) {
        if (error) *error = why.empty() ? "cannot be decoded" : why;
        return std::nullopt;
    }
    return image;
}

std::optional<Image> CharacterLibrary::texture_map(std::string_view name, TextureMap map) const {
    if (name.starts_with("layers:")) return std::nullopt;
    return decode_texture_map(name, read_file(name), map, true);
}

std::vector<std::string> CharacterLibrary::eqg_model_codes() const {
    Lock lock(*mutex_);
    return eqg_ ? eqg_->codes() : std::vector<std::string>{};
}

std::optional<Image> CharacterLibrary::layered_texture(std::string_view key, std::string* error) const {
    if (!key.starts_with("layers:")) return std::nullopt;
    std::string_view body = key.substr(7);
    std::uint32_t tint = 0;
    if (auto hash = body.rfind('#'); hash != std::string_view::npos) {
        // Hex, at most 32 bits (strtoul's long differs by platform); a bad or longer tint is 0.
        auto hex = body.substr(hash + 1);
        if (std::from_chars(hex.data(), hex.data() + hex.size(), tint, 16).ec != std::errc{}) tint = 0;
        body = body.substr(0, hash);
    }
    std::vector<Image> layers;
    std::uint32_t w = 0, h = 0;
    for (std::size_t p = 0; p <= body.size();) {
        std::size_t q = std::min(body.find('+', p), body.size());
        std::string name(body.substr(p, q - p));
        p = q + 1;
        auto data = read_file(name);
        std::string why;
        auto img = decode_game_texture(name, data, {.wld_bmp_rows = true}, &why);
        if (!img) {
            if (error) *error = "layer " + name + ": " + (why.empty() ? "cannot be decoded" : why);
            return std::nullopt;
        }
        Image rgba = to_rgba8(*img);
        rgba.mips.resize(1);
        // Composited at the size of the layer with the most texels (a texture pack may upscale one layer): not the
        // widest by the tallest, which a wide and a tall layer would make 16384 x 16384.
        if (std::uint64_t{rgba.width} * rgba.height > std::uint64_t{w} * h) {
            w = rgba.width;
            h = rgba.height;
        }
        layers.push_back(std::move(rgba));
    }
    if (layers.empty() || w == 0 || h == 0) {
        if (error) *error = "no layers";
        return std::nullopt;
    }
    Image out;
    out.width = w;
    out.height = h;
    out.mips.emplace_back(static_cast<std::size_t>(w) * h * 4, std::byte{0});
    auto& dst = out.mips.front();
    const bool tinted = (tint >> 24) != 0;
    const std::uint32_t tc[3] = {(tint >> 16) & 0xffu, (tint >> 8) & 0xffu, tint & 0xffu};
    for (std::size_t li = 0; li < layers.size(); ++li) {
        const Image& src = layers[li];
        const auto& s = src.mips.front();
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                // Nearest sample when a layer's size differs from the composite's.
                std::size_t sx = static_cast<std::size_t>(x) * src.width / w;
                std::size_t sy = static_cast<std::size_t>(y) * src.height / h;
                const std::byte* sp = &s[(sy * src.width + sx) * 4];
                std::byte* dp = &dst[(static_cast<std::size_t>(y) * w + x) * 4];
                std::uint32_t alpha = li == 0 ? 255u : std::to_integer<std::uint32_t>(sp[3]);
                for (int c = 0; c < 3; ++c) {
                    std::uint32_t v = std::to_integer<std::uint32_t>(sp[c]);
                    if (li > 0 && tinted) v = v * tc[c] / 255u;
                    std::uint32_t d = std::to_integer<std::uint32_t>(dp[c]);
                    dp[c] = static_cast<std::byte>((v * alpha + d * (255u - alpha) + 127u) / 255u);
                }
                dp[3] = std::byte{255};
            }
    }
    return out;
}

void CharacterLibrary::apply_npc_tint(const CharacterModel& model, const game::Appearance& a, CharacterMesh& mesh) const {
    // ActorClient_ApplyNpcTint @0x40cea0, called for new-style model races
    // (NpcManager_IsNewStyleModelRace): npct.ini Value<index> on the whole
    // actor (SetTint without children, so not the held items).
    const game::RaceModel* rm = game::find_model_code(model.code);
    if (!appearance_ || !rm || !(rm->flags & game::kNewStyleModel)) return;
    auto entry = game::npc_tint_entry(static_cast<std::int32_t>(a.npc_tint),
                                      static_cast<std::int32_t>(appearance_->npc_tints.values.size()));
    const std::uint32_t* color = entry ? appearance_->npc_tints.entry(*entry) : nullptr;
    if (!color) return;
    const std::uint32_t t = (*color & 0xffffffu) == 0 ? 0u : (*color | 0xff000000u);
    std::fill(mesh.tints.begin(), mesh.tints.end(), t);
}

CharacterMesh CharacterLibrary::dress_eqg(const CharacterModel& model, const game::Appearance& a) const {
    // Body piece <code><TextureType>, head piece <code>HE<HeadType> (HNM:
    // HNM00/HNM07, HNMHE00..HNMHE07), then the "_sNN_" texture variant.
    std::uint32_t variant = a.texture == game::kNoTexture ? 0 : a.texture;
    CharacterMesh out;
    auto file = eqg_ ? eqg_->mesh_file(model.code) : nullptr;
    if (file) {
        eqg_character::append_pieces(*file, eqg_character::select_pieces(*file, model.code, variant, a.helm), out);
    }
    if (out.indices.empty()) out = model.default_mesh();
    // New models of the classic races (Drakkin): customization features,
    // armour parts and hero's forge (character_parts.cpp).
    if (const game::RaceModel* rm = game::find_model_code(model.code);
        rm && game::classic_head_race(rm->race) && a.texture == game::kNoTexture) {
        dress_new_model(model, a, out);
        return out;
    }
    // The client's path for new-style models: PlayerClient::SwapNPCMaterials
    // @0x594ad0 (TextureType 0..30) -> SwapNpcMaterials @0x10043630 ->
    // SwapNpcMaterialsNewModel @0x100433d0: material i (file order) takes
    // layer "C_<code>_S<TextureType>_M<i + 1>" when one is loaded, and keeps
    // its own textures otherwise. Layers are the model's .lay (EQGL) entries,
    // so texture 4 of GUS is c_gus_bk_s04_c.dds on the body and the debris,
    // not c_gus_bd_s04_c.dds.
    if (file && a.texture != game::kNoTexture && a.texture <= 30) {
        bool layered = false;
        for (std::size_t i = 0; i < file->materials.size(); ++i) {
            const eqg_character::Layer* layer = eqg_->layer(eqg_character::layer_name(model.code, a.texture, i));
            if (!layer) continue;
            layered = true;
            if (layer->diffuse.empty()) continue;
            for (auto& mat : out.materials)
                if (mat.name == file->materials[i].name) mat.frames = {layer->diffuse};
        }
        if (layered) return out;
    }
    // Models without layers for the variant: the "_sNN" texture of that
    // number where the file exists (not seen in the client; docs/unknowns.md).
    if (variant == 0 || variant >= 100) return out;
    for (auto& mat : out.materials)
        for (auto& frame : mat.frames) {
            std::string l = lower(frame);
            auto p = l.find("_s");
            if (p == std::string::npos || p + 4 > l.size() || !all_digits(l.substr(p + 2, 2))) continue;
            std::string c = l.substr(0, p + 2) + two_digits(variant) + l.substr(p + 4);
            if (has_file(c)) frame = c;
        }
    return out;
}

void CharacterLibrary::attach_items(const CharacterModel& model, const game::Appearance& a, CharacterMesh& mesh) const {
    // SetHeldModel(0, "IT<n>") from SwapHead: wear slot 0 is the actor's part
    // 1, the "<code>HE_DAG" point (CDisplay::UpdateDAGPointers).
    const std::uint32_t head_item = model.eqg || model.luclin ? 0u : head_of(model, a).item;
    for (int slot : {game::kSlotHead, game::kSlotPrimary, game::kSlotSecondary}) {
        std::uint32_t number = slot == game::kSlotHead ? head_item : a.material[static_cast<std::size_t>(slot)];
        if (number == 0) continue;
        auto held = item_mesh(number);
        if (!held) continue;
        const CharacterMesh* item = &held->mesh;
        // PlayerClient::SetHeldModel puts a secondary on the shield point when CDisplay::IsShield knows its model
        // number, whatever its textures are called.
        const bool shield = slot == game::kSlotSecondary && game::is_shield_model(number);
        int bone = -1;
        if (model.eqg) {
            const char* names[] = {shield ? "ARML_SHLD" : (slot == game::kSlotSecondary ? "ARML_WEAP" : "ARMR_WEAP"),
                                   slot == game::kSlotSecondary ? "ARML_WEAP" : "ARMR_WEAP"};
            for (const char* nm : names) {
                for (std::size_t i = 0; i < model.bone_names.size() && bone < 0; ++i)
                    if (upper(model.bone_names[i]) == nm) bone = static_cast<int>(i);
                if (bone >= 0) break;
            }
        } else if (slot == game::kSlotHead) {
            bone = model.find_bone_prefix("HE");
        } else {
            // SetBone binds "<code>R_POINT_DAG", "<code>L_POINT_DAG" or
            // "<code>SHIELD_POINT_DAG" (slots 3, 4, 5), matched by prefix.
            if (shield) bone = model.find_bone_prefix("SHIELD_POINT");
            if (bone < 0) bone = model.find_bone_prefix(slot == game::kSlotSecondary ? "L_POINT" : "R_POINT");
        }
        if (bone < 0) continue;
        // The item sits in the bone's space with no extra matrix
        // (ChangeBoneAttachment @0x10042c60 gets matrix 0 for held items).
        // A simple-mesh item's world matrix is exactly the bone's world
        // matrix: CSimpleActorForAttachment skips the definition scale and
        // CSimpleActor::SetParentMatrix @0x10052bc0 computes Local(identity)
        // x Attach(identity) x the bone's accumulated matrix. Skinning the
        // item's vertices to the bone, unscaled, gives the same.
        // EQG vertices are in bind space, so the bind matrix takes the item
        // there; WLD vertices are already in bone space.
        glm::mat4 to_model(1.0f); // item (grip) space -> the vertex space of `bone`
        if (static_cast<std::size_t>(bone) < model.inverse_bind.size())
            to_model = glm::inverse(model.inverse_bind[static_cast<std::size_t>(bone)]);
        auto vbase = static_cast<std::uint32_t>(mesh.vertices.size());
        auto mbase = static_cast<std::uint32_t>(mesh.materials.size());
        for (auto v : item->vertices) {
            v.position = glm::vec3(to_model * glm::vec4(v.position, 1.0f));
            v.normal = glm::mat3(to_model) * v.normal;
            v.bones = {static_cast<std::uint16_t>(bone), 0, 0, 0};
            v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
            mesh.vertices.push_back(v);
        }
        // Held items: SetHeldModel tints them with itmt.ini[Primary /
        // SecondaryTintIndex] (index as-is); NMS has no itmt.ini, so ColorTable
        // GetEntry is always null and no held item is tinted. A helm item
        // (slot 0) takes the head's armour colour with alpha 0xff through
        // CDisplay::ChangeDag (zero RGB: untinted).
        const std::uint32_t head_tint = a.tint[game::kSlotHead];
        const std::uint32_t item_tint =
            slot == game::kSlotHead && (head_tint & 0xffffffu) != 0 ? (head_tint | 0xff000000u) : 0u;
        for (const auto& m : item->materials) {
            mesh.materials.push_back(m);
            mesh.tints.push_back(item_tint);
        }
        // The renderer turns the front face for EQG models, so an item wound the other way round from the body
        // has each triangle reversed (else only its inside faces would show).
        const bool reverse = held->eqg != model.eqg;
        for (auto p : item->parts) {
            MeshPart q;
            q.first_index = static_cast<std::uint32_t>(mesh.indices.size());
            q.index_count = p.index_count;
            q.material = mbase + p.material;
            for (std::uint32_t k = 0; k < p.index_count; ++k)
                mesh.indices.push_back(vbase + item->indices[p.first_index + k]);
            if (reverse)
                for (std::size_t k = q.first_index; k + 2 < mesh.indices.size(); k += 3)
                    std::swap(mesh.indices[k + 1], mesh.indices[k + 2]);
            mesh.parts.push_back(q);
        }
        // Its always-on actor emitters: on its points, whose bone is the item's own origin (ATTACH_TO_ORIGIN),
        // i.e. the frame of the bone it is held by.
        for (const auto& e : held->emitters)
            if (const ModelPoint* p = find_point(held->points, e.point))
                mesh.emitters.push_back({bone, point_matrix(*p), e, "IT" + std::to_string(number)});
    }
}

} // namespace hailstone::assets
