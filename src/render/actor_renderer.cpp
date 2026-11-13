#include "render/actor_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/texture.h"
#include "core/frame_sections.h"
#include "core/log.h"
#include "core/work_budget.h"
#include "game/anim_layers.h"
#include "game/appearance.h"
#include "game/appearance_data.h"
#include "game/combat.h"
#include "game/coords.h"
#include "game/mechanics/anim_select.h"
#include "game/race_models.h"
#include "game/spawn_animation.h"
#include "game/sine_table.h"
#include "game/view_distance.h"
#include "game/world_state.h"
#include "render/culling.h"
#include "render/look_eviction.h"
#include "render/material_state.h"
#include "render/vk_util.h"

namespace hailstone::render {

namespace {

// Frames an appearance may wait for a texture another appearance job is reading (prepare_spawns), about half a second.
constexpr int kMaxDressDefer = 30;

struct GpuVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    std::array<std::uint16_t, 4> bones;
    glm::vec4 weights;
};

// Matches the push_constant block in actor.vert / actor.frag.
struct PushConstants {
    glm::mat4 view_proj;
    glm::vec4 fog_color;
    glm::vec4 fog; // start, end, alpha_ref, opacity
    glm::vec4 camera_pos;
    std::uint32_t texture;
    std::uint32_t bone_offset;
    std::uint32_t tint; // 0xAARRGGBB; high byte 0 = none
    // SkinMeshLuclin's second texture, 0xffffffff = none. In the colour pass the high 16 bits of `texture` carry a
    // texture pack's roughness map slot and those of `texture2` its normal map slot (0 = none; 0xffff in texture2's
    // low bits = no second texture).
    std::uint32_t texture2;
};
static_assert(sizeof(PushConstants) <= 128);

VkShaderModule load_shader(VkDevice device, const std::filesystem::path& path) {
    return create_shader_module(device, path);
}

// Cross-fades: CBoneGroup::BlendToAnimation @0x1003fcc0 fades the new track
// in and the old one out linearly over (new.blendIn + current.blendOut) / 2
// ms (PlayerAnimation_SelectMovementAnim @0x5890d8, table 0xaae3d8): every
// record is 100/100 except STND and IDLE (blendIn 250), so 100 ms, or 175 ms
// into STND (P01) / IDLE (O01). The clip
// and rate come from game/spawn_animation.h.

} // namespace

ActorRenderer::ActorRenderer(VkContext& vk, std::shared_ptr<const assets::CharacterLibrary> library,
                             const std::filesystem::path& shader_dir, std::unique_ptr<Kept> kept)
    : vk_(vk), library_(std::move(library)) {
    VkDevice d = vk_.device();
    // A texture pack gives a material up to three slots (its colour, normal and roughness maps), and a busy zone (the
    // bazaar) has well over a thousand character materials; the slots go to the shaders in 16 bits (0xffff: none).
    max_textures_ = std::max(1u, std::min(0xffffu, vk_.caps().max_sampled_images));

    for (std::size_t i = 0; i < bones_.size(); ++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(glm::mat4) * kMaxBones;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        vk_check(vmaCreateBuffer(vk_.allocator(), &bi, &ai, &bones_[i].buffer, &bones_[i].allocation, &out),
                 "create bone buffer");
        bones_[i].size = bi.size;
        bones_mapped_[i] = out.pMappedData;
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vk_.caps().max_anisotropy > 0.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(8.0f, vk_.caps().max_anisotropy);
    }
    vk_check(vkCreateSampler(d, &si, nullptr, &sampler_), "vkCreateSampler");

    // Binding 1 is the lighting (render/scene_lighting.h), 2 the textures: a variable-count array must have the
    // highest binding number.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = max_textures_;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags flags[3] = {0, 0,
                                         VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                             VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                             vk_.texture_array_binding_flags()};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 3;
    bf.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bf;
    li.flags = vk_.texture_array_layout_flags();
    li.bindingCount = 3;
    li.pBindings = bindings;
    vk_check(vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * VkContext::kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_textures_ * VkContext::kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = vk_.texture_array_pool_flags();
    pi.maxSets = VkContext::kFramesInFlight;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    vk_check(vkCreateDescriptorPool(d, &pi, nullptr, &pool_), "vkCreateDescriptorPool");
    for (auto& set : sets_) {
        VkDescriptorSetVariableDescriptorCountAllocateInfo vc{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        vc.descriptorSetCount = 1;
        vc.pDescriptorCounts = &max_textures_;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.pNext = &vc;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        vk_check(vkAllocateDescriptorSets(d, &ai, &set), "vkAllocateDescriptorSets");
    }
    lighting_buffers_ = std::make_unique<FrameBuffers>(vk_, sizeof(SceneLighting), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (std::size_t i = 0; i < sets_.size(); ++i) {
        VkDescriptorBufferInfo infos[2] = {{bones_[i].buffer, 0, VK_WHOLE_SIZE},
                                           {lighting_buffers_->buffer(i), 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[2]{};
        for (std::uint32_t k = 0; k < 2; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = sets_[i];
            w[k].dstBinding = k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[k].pBufferInfo = &infos[k];
        }
        vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
    }

    if (kept) adopt(*kept);
    // Texture slot 0 is plain white, for anything that fails to load.
    if (textures_.empty()) {
        UploadBatch upload(vk_);
        assets::Image white;
        white.width = white.height = 1;
        white.mips.push_back(std::vector<std::byte>(4, std::byte{0xff}));
        textures_.push_back(upload.immutable_texture(white));
        upload.submit();
    }
    for (std::uint32_t slot = 0; slot < VkContext::kFramesInFlight; ++slot) rebuild_descriptors(slot); // none in flight yet
    create_pipelines(shader_dir);
}

ActorRenderer::~ActorRenderer() {
    stop_dresser();
    VkDevice d = vk_.device();
    vk_.wait_idle();
    for (VkPipeline p : pipelines_)
        if (p) vkDestroyPipeline(d, p, nullptr);
    if (shadow_pipeline_) vkDestroyPipeline(d, shadow_pipeline_, nullptr);
    if (silhouette_pipeline_) vkDestroyPipeline(d, silhouette_pipeline_, nullptr);
    if (point_shadow_pipeline_) vkDestroyPipeline(d, point_shadow_pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
    for (auto& t : textures_) t.destroy(d, vk_.allocator());
    for (auto& [m, g] : models_) free_model(g);
    for (auto& b : bones_) b.destroy(vk_.allocator());
}

void ActorRenderer::free_model(GpuModel& g) {
    g.vertices.destroy(vk_.allocator());
    g.indices.destroy(vk_.allocator());
}

void ActorRenderer::retire_model(GpuModel& g) {
    Buffer vertices = std::exchange(g.vertices, Buffer{});
    Buffer indices = std::exchange(g.indices, Buffer{});
    vk_.retire([allocator = vk_.allocator(), vertices, indices]() mutable {
        vertices.destroy(allocator);
        indices.destroy(allocator);
    });
}

ActorRenderer::Kept::~Kept() {
    if (!vk || (models.empty() && textures.empty())) return;
    vk->wait_idle();
    for (auto& t : textures) t.destroy(vk->device(), vk->allocator());
    for (auto& kv : models) {
        kv.second.vertices.destroy(vk->allocator());
        kv.second.indices.destroy(vk->allocator());
    }
}

namespace {

// The texture slots a mesh's parts draw with.
template <class Parts, class Fn>
void for_each_slot(const Parts& parts, Fn&& fn) {
    for (const auto& part : parts)
        for (std::uint32_t t : {part.texture, part.texture2, part.normal_map, part.rough_map})
            if (t != 0xffffffffu) fn(t);
}

} // namespace

std::unique_ptr<ActorRenderer::Kept> ActorRenderer::release_kept() {
    stop_dresser(); // before the next zone's library is made (CharacterArchiveCache::library_for_zone)
    vk_.wait_idle();
    auto kept = std::make_unique<Kept>();
    kept->vk = &vk_;
    // Only what the libraries' shared cache holds can be handed out the same in another zone (a zone archive's model
    // or texture never is), and of it only what was drawn here: memory stays about one zone's worth.
    std::unordered_map<const assets::CharacterMesh*, const assets::CharacterModel*> default_of;
    for (const auto& kv : default_meshes_)
        if (kv.second) default_of.emplace(kv.second.get(), kv.first);
    std::unordered_set<std::uint32_t> slots{0};
    for (auto& kv : models_) {
        const assets::CharacterMesh* mesh = kv.first;
        GpuModel& g = kv.second;
        auto d = default_of.find(mesh);
        const bool shared = d != default_of.end() ? model_refs_.contains(d->second) && library_->shares(d->second)
                                                  : mesh_refs_.contains(mesh) && library_->shares(mesh);
        if (!g.used || !shared || g.capacity_blocked) {
            free_model(g);
            continue;
        }
        for_each_slot(g.parts, [&](std::uint32_t t) { slots.insert(t); });
        g.used = false;
        if (d != default_of.end()) {
            kept->default_meshes.emplace(d->second, default_meshes_[d->second]);
            kept->model_refs.emplace(d->second, model_refs_[d->second]);
        } else {
            kept->mesh_refs.emplace(mesh, mesh_refs_[mesh]);
        }
        kept->models.emplace(mesh, std::move(g));
    }
    for (std::uint32_t i = 1; i < textures_.size(); ++i)
        if (!slots.contains(i)) textures_[i].destroy(vk_.device(), vk_.allocator());
    // A texture that did not load (slot 0) is tried again in the next zone.
    for (auto& kv : texture_slots_)
        if (kv.second.slot != 0 && slots.contains(kv.second.slot)) kept->texture_slots.insert(kv);
    kept->textures = std::move(textures_);
    textures_.clear();
    texture_slots_.clear();
    free_textures_.clear();
    models_.clear();
    mesh_refs_.clear();
    default_meshes_.clear();
    model_refs_.clear();
    spawns_.clear();
    looks_.clear();
    claims_.reset();
    by_code_.clear();
    prepared_.clear();
    prepared_ids_.clear();
    last_draws_.clear();
    return kept;
}

void ActorRenderer::adopt(Kept& kept) {
    textures_ = std::move(kept.textures);
    texture_slots_ = std::move(kept.texture_slots);
    models_ = std::move(kept.models);
    mesh_refs_ = std::move(kept.mesh_refs);
    default_meshes_ = std::move(kept.default_meshes);
    model_refs_ = std::move(kept.model_refs);
    kept.textures.clear();
    kept.texture_slots.clear();
    kept.models.clear();
    kept.mesh_refs.clear();
    kept.default_meshes.clear();
    kept.model_refs.clear();
    // This zone's own archives may give a texture or a mesh another look: those are read and built again.
    std::unordered_set<std::uint32_t> stale;
    for (auto it = texture_slots_.begin(); it != texture_slots_.end();) {
        if (library_->unaffected(it->second.lookups)) {
            ++it;
            continue;
        }
        stale.insert(it->second.slot);
        it = texture_slots_.erase(it);
    }
    std::unordered_map<const assets::CharacterMesh*, const assets::CharacterModel*> default_of;
    for (const auto& kv : default_meshes_)
        if (kv.second) default_of.emplace(kv.second.get(), kv.first);
    std::unordered_set<std::uint32_t> slots{0};
    for (auto it = models_.begin(); it != models_.end();) {
        const assets::CharacterMesh* mesh = it->first;
        auto d = default_of.find(mesh);
        bool keep = d != default_of.end() ? library_->shares(d->second) : library_->shares(mesh);
        for_each_slot(it->second.parts, [&](std::uint32_t t) { keep = keep && !stale.contains(t); });
        if (keep) {
            for_each_slot(it->second.parts, [&](std::uint32_t t) { slots.insert(t); });
            ++it;
            continue;
        }
        free_model(it->second);
        if (d != default_of.end()) {
            default_meshes_.erase(d->second);
            model_refs_.erase(d->second);
        } else {
            mesh_refs_.erase(mesh);
        }
        it = models_.erase(it);
    }
    for (std::uint32_t i = 1; i < textures_.size(); ++i) {
        if (!slots.contains(i)) textures_[i].destroy(vk_.device(), vk_.allocator());
        if (!textures_[i].image) free_textures_.push_back(i);
    }
    std::erase_if(texture_slots_, [&](const auto& kv) { return !slots.contains(kv.second.slot); });
    // Lowest first: the array's end stays free for the descriptor writes.
    std::sort(free_textures_.rbegin(), free_textures_.rend());
    stats_.models = models_.size();
    log::info("character models kept from the last zone: {} meshes, {} textures", models_.size(),
              textures_.size() - 1 - free_textures_.size());
}

void ActorRenderer::rebuild_descriptors(std::uint32_t slot) {
    // Only the slot's own set: its last frame's fence has passed (VkContext::begin_frame), while the other slot's
    // frame may still be in flight. The other set takes the new textures when its frame comes round, so a new model
    // no longer waits for the GPU to go idle.
    auto& written = descriptor_textures_[slot];
    if (written == textures_.size()) return;
    std::vector<VkDescriptorImageInfo> infos;
    for (std::size_t i = written; i < textures_.size(); ++i) // a freed slot: plain white
        infos.push_back({sampler_, textures_[i].view ? textures_[i].view : textures_[0].view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets_[slot];
    w.dstBinding = 2;
    w.dstArrayElement = written;
    w.descriptorCount = static_cast<std::uint32_t>(infos.size());
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = infos.data();
    vkUpdateDescriptorSets(vk_.device(), 1, &w, 0, nullptr);
    written = static_cast<std::uint32_t>(textures_.size());
    stats_.textures = textures_.size() - free_textures_.size();
}

VkDescriptorSet ActorRenderer::frame_set(const FrameInfo& frame) {
    // Textures are added only by prepare, before the frame's first bind; this catches a frame drawn without one.
    rebuild_descriptors(frame.frame_slot);
    return sets_[frame.frame_slot];
}

std::optional<std::uint32_t> ActorRenderer::add_texture(UploadBatch& upload, const assets::Image& image,
                                                        TextureUse use) {
    if (!free_textures_.empty()) {
        const std::uint32_t slot = free_textures_.back();
        free_textures_.pop_back();
        textures_[slot] = upload.immutable_texture(image, use);
        // Each set takes it when its own frame comes round (rebuild_descriptors).
        for (auto& written : descriptor_textures_) written = std::min(written, slot);
        return slot;
    }
    if (textures_.size() >= max_textures_) return std::nullopt;
    textures_.push_back(upload.immutable_texture(image, use));
    return static_cast<std::uint32_t>(textures_.size() - 1);
}

std::string ActorRenderer::TextureRef::key() const {
    if (map) return name + (*map == assets::TextureMap::Normal ? "#n" : "#r");
    return name + (masked ? "#m" : "") + (use == TextureUse::OpaqueColor ? "#o" : "");
}

ActorRenderer::ReadTexture ActorRenderer::read_texture(const assets::CharacterLibrary& library, const TextureRef& ref) {
    ReadTexture t;
    {
        assets::CharacterLookups rec; // what the image depends on, for the next zone (adopt)
        t.image = ref.map ? library.texture_map(ref.name, *ref.map) : library.texture(ref.name, ref.masked, &t.error);
        t.lookups = rec.names();
    }
    t.use = ref.map ? TextureUse::Data : ref.use;
    // UploadBatch::texture's CPU half, here: on the worker, the decoding and the mips stay off the main thread.
    if (t.image)
        if (auto prepared = prepare_texture_image(*t.image, t.use)) {
            t.image = std::move(prepared);
            t.use = TextureUse::Data;
        }
    return t;
}

std::uint32_t ActorRenderer::texture_slot(const std::string& name, bool masked, UploadBatch& upload, TextureUse use) {
    MeshPlan none;
    return slot_for({name, masked, use, std::nullopt}, none, upload);
}

std::uint32_t ActorRenderer::map_slot(const std::string& name, assets::TextureMap map, UploadBatch& upload) {
    MeshPlan none;
    return slot_for({name, false, TextureUse::Data, map}, none, upload);
}

std::uint32_t ActorRenderer::slot_for(const TextureRef& ref, MeshPlan& plan, UploadBatch& upload) {
    std::string key = ref.key();
    if (auto it = texture_slots_.find(key); it != texture_slots_.end()) return it->second.slot;
    ReadTexture t;
    if (auto it = plan.textures.find(key); it != plan.textures.end()) {
        t = std::move(it->second);
    } else {
        // Read here: a default mesh, the model viewer's draws, or (after prepare_spawns waited its frames for it) a
        // texture another job claimed.
        t = read_texture(*library_, ref);
    }
    if (claims_) claims_->installed(key); // has its slot from here on: no job reads it again
    std::uint32_t slot = 0;
    bool capacity_blocked = false;
    if (ref.map) {
        if (t.image) {
            if (auto added = add_texture(upload, *t.image, t.use)) slot = *added;
            else capacity_blocked = true;
        }
    } else {
        std::optional<std::uint32_t> added = t.image ? add_texture(upload, *t.image, t.use) : std::nullopt;
        if (added) {
            slot = *added;
        } else {
            capacity_blocked = t.image.has_value();
            // Drawn with slot 0 (plain white), which shows as an untextured grey model: say why, once per texture.
            ++stats_.missing_textures;
            log::warn("character texture {}: {}", ref.name, t.image ? "texture array full" : t.error);
        }
    }
    texture_slots_.emplace(std::move(key), TextureSlot{slot, std::move(t.lookups), capacity_blocked});
    return slot;
}

const assets::CharacterMesh& ActorRenderer::default_mesh(const assets::CharacterModel& model) {
    auto& m = default_meshes_[&model];
    if (!m) m = std::make_shared<const assets::CharacterMesh>(model.default_mesh());
    return *m;
}

ActorRenderer::MeshPlan ActorRenderer::plan_mesh(const assets::CharacterMesh& model) {
    MeshPlan plan;
    std::vector<GpuVertex> vertices;
    vertices.reserve(model.vertices.size());
    for (const auto& v : model.vertices) vertices.push_back({v.position, v.normal, v.uv, v.bones, v.weights});
    auto bytes = std::as_bytes(std::span(vertices));
    plan.vertices.assign(bytes.begin(), bytes.end());
    for (const auto& p : model.parts) {
        const auto& m = model.materials[p.material];
        if (m.blend == assets::BlendMode::Invisible || m.frames.empty()) continue;
        MeshPlan::PartPlan pp;
        Part& part = pp.part;
        part.first_index = p.first_index;
        part.index_count = p.index_count;
        part.tint = p.material < model.tints.size() ? model.tints[p.material] : 0;
        part.two_sided = m.two_sided;
        MaterialState st = material_state(m);
        pp.texture = {m.frames.front(), st.masked_texture,
                      st.alpha_unused() ? TextureUse::OpaqueColor : TextureUse::Color, std::nullopt};
        if (!m.tint_layer.empty()) pp.texture2 = TextureRef{m.tint_layer, false, TextureUse::Color, std::nullopt};
        pp.normal = {m.frames.front(), false, TextureUse::Data, assets::TextureMap::Normal};
        pp.rough = {m.frames.front(), false, TextureUse::Data, assets::TextureMap::Roughness};
        part.pass = static_cast<Pass>(st.pass);
        part.alpha_ref = st.alpha_ref;
        part.opacity = st.opacity;
        part.fog = st.fog;
        plan.parts.push_back(std::move(pp));
    }
    return plan;
}

const ActorRenderer::GpuModel* ActorRenderer::gpu_model(const assets::CharacterMesh& model, UploadBatch& upload) {
    if (auto it = models_.find(&model); it != models_.end()) {
        it->second.used = true;
        it->second.last_frame = vk_.frames_begun();
        it->second.last_ms = now_ms_;
        return &it->second;
    }
    MeshPlan plan = plan_mesh(model);
    return install(model, plan, upload);
}

const ActorRenderer::GpuModel* ActorRenderer::install(const assets::CharacterMesh& model, MeshPlan& plan,
                                                      UploadBatch& upload) {
    if (auto it = models_.find(&model); it != models_.end()) {
        it->second.used = true;
        it->second.last_frame = vk_.frames_begun();
        it->second.last_ms = now_ms_;
        return &it->second;
    }
    GpuModel g;
    const auto resolve = [&](const TextureRef& ref) {
        const auto slot = slot_for(ref, plan, upload);
        g.capacity_blocked = g.capacity_blocked || texture_slots_.at(ref.key()).capacity_blocked;
        return slot;
    };
    for (auto& pp : plan.parts) {
        Part part = pp.part;
        part.texture = resolve(pp.texture);
        if (pp.texture2) part.texture2 = resolve(*pp.texture2);
        part.normal_map = resolve(pp.normal);
        part.rough_map = resolve(pp.rough);
        g.parts.push_back(part);
    }
    std::stable_sort(g.parts.begin(), g.parts.end(), [](const Part& a, const Part& b) { return a.pass < b.pass; });
    g.used = true;
    g.last_frame = vk_.frames_begun();
    g.last_ms = now_ms_;
    g.vertices = upload.buffer(plan.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    g.indices = upload.buffer(std::as_bytes(std::span(model.indices)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    stats_.models = models_.size() + 1;
    return &models_.emplace(&model, std::move(g)).first->second;
}

bool ActorRenderer::waits_for_claimed(const MeshPlan& plan) const {
    if (!claims_) return false;
    auto waits = [&](const TextureRef& ref) {
        const std::string key = ref.key();
        return !texture_slots_.contains(key) && !plan.textures.contains(key) && claims_->pending(key);
    };
    for (const auto& pp : plan.parts)
        if (waits(pp.texture) || (pp.texture2 && waits(*pp.texture2)) || waits(pp.normal) || waits(pp.rough))
            return true;
    return false;
}

ActorRenderer::Dressing ActorRenderer::dress_off_thread(const assets::CharacterLibrary& library,
                                                        std::shared_ptr<const assets::CharacterModel> model,
                                                        const game::Appearance& appearance,
                                                        std::shared_ptr<TextureClaims> claims) {
    Dressing d;
    d.held = ClaimHold(std::move(claims));
    d.model = std::move(model);
    d.appearance = appearance;
    d.mesh = library.dress(*d.model, appearance);
    if (!d.mesh) return d;
    d.plan = plan_mesh(*d.mesh);
    auto read = [&](const TextureRef& ref) {
        std::string key = ref.key();
        if (!d.plan->textures.contains(key) && d.held.claim(key)) d.plan->textures.emplace(key, read_texture(library, ref));
    };
    for (const auto& pp : d.plan->parts) {
        read(pp.texture);
        if (pp.texture2) read(*pp.texture2);
        read(pp.normal);
        read(pp.rough);
    }
    return d;
}

void ActorRenderer::request_dress(std::uint32_t id, const std::shared_ptr<const assets::CharacterModel>& model,
                                  const game::Appearance& appearance) {
    if (!dresser_) {
        claims_ = std::make_shared<TextureClaims>();
        for (const auto& kv : texture_slots_) claims_->installed(kv.first); // the textures kept from the last zone
        dresser_ = std::make_unique<core::LatestJobs<std::uint32_t, Dressing>>();
    }
    // The job holds its own references (the library, the model, the claims), never the renderer.
    dresser_->submit(id, [library = library_, model, appearance, claims = claims_] {
        return dress_off_thread(*library, model, appearance, claims);
    });
}

void ActorRenderer::stop_dresser() {
    if (dresser_) dresser_->stop();
    dresser_.reset();
    ready_.clear();
}

void ActorRenderer::evict_idle_looks() {
    if (static_cast<std::uint32_t>(now_ms_ - last_look_sweep_ms_) < kLookSweepMs) return;
    last_look_sweep_ms_ = now_ms_;
    // A look a spawn's state wears stays, a hidden spawn's too (worn_meshes).
    const auto worn = worn_meshes(spawns_);
    const std::uint64_t frame = vk_.frames_begun();
    std::size_t evicted = 0;
    for (auto it = looks_.begin(); it != looks_.end();) {
        std::erase_if(it->second.looks, [&](const auto& look) {
            const assets::CharacterMesh* mesh = look.second.get();
            if (!mesh || worn.contains(mesh)) return false; // a null look (the default geometry) costs nothing
            if (auto g = models_.find(mesh); g != models_.end()) {
                if (!look_idle(g->second.last_frame, g->second.last_ms, frame, now_ms_, VkContext::kFramesInFlight))
                    return false;
                // Clock age selects the cache entry; the GPU completion point,
                // rather than a count of begun frames, governs destruction.
                retire_model(g->second);
                models_.erase(g);
            }
            mesh_refs_.erase(mesh);
            ++evicted;
            return true;
        });
        it = it->second.looks.empty() ? looks_.erase(it) : std::next(it);
    }
    // Texture slots are reclaimed after an eviction; a capacity retry is tried every sweep while slots are free.
    if (evicted || !free_textures_.empty()) evict_unused_textures(evicted != 0);
    if (evicted) {
        stats_.models = models_.size();
        log::debug("freed {} idle appearances ({} meshes on the GPU)", evicted, models_.size());
    }
}

void ActorRenderer::evict_unused_textures(bool reclaim_first) {
    auto reclaim = [&] {
        const auto used = model_texture_slots(models_);
        for (auto it = texture_slots_.begin(); it != texture_slots_.end();) {
            if (it->second.slot == 0 || used.contains(it->second.slot)) {
                ++it;
                continue;
            }
            if (claims_) claims_->evicted(it->first);
            it = texture_slots_.erase(it);
        }
        for (std::uint32_t slot = 1; slot < textures_.size(); ++slot) {
            if (!textures_[slot].image || used.contains(slot)) continue;
            Texture old = std::exchange(textures_[slot], Texture{});
            // Each descriptor set is rewritten only after its frame fence. Older
            // frames keep the old image alive through this completion point, even
            // when a subsequent upload reuses the numeric slot immediately.
            vk_.retire([device = vk_.device(), allocator = vk_.allocator(), old]() mutable {
                old.destroy(device, allocator);
            });
            free_textures_.push_back(slot);
            for (auto& written : descriptor_textures_) written = std::min(written, slot);
        }
    };
    if (reclaim_first) reclaim();
    if (std::none_of(models_.begin(), models_.end(), [](const auto& kv) { return kv.second.capacity_blocked; })) {
        stats_.textures = textures_.size() - free_textures_.size();
        return;
    }
    // The GPU models spawns wear (a default mesh included): rebuilt at once when discarded, so retried a few a sweep.
    std::unordered_set<const assets::CharacterMesh*> worn;
    for (const auto& kv : spawns_) {
        const SpawnState& st = kv.second;
        if (st.mesh) worn.insert(st.mesh.get());
        else if (st.model)
            if (auto d = default_meshes_.find(st.model.get()); d != default_meshes_.end() && d->second)
                worn.insert(d->second.get());
    }
    if (retry_capacity_blocked_textures(models_, worn, kMaxWornRetries, texture_slots_, !free_textures_.empty(),
            [this](GpuModel& model) { retire_model(model); },
            [this](const std::string& key) { if (claims_) claims_->evicted(key); })) {
        // The invalidated models may have owned successful uploads too. They
        // are no longer referenced; make that capacity available for the retry.
        reclaim();
    }
    stats_.textures = textures_.size() - free_textures_.size();
}

const std::shared_ptr<const assets::CharacterMesh>* ActorRenderer::applied_look(const assets::CharacterModel& model,
                                                                                const game::Appearance& a) const {
    auto it = looks_.find(&model);
    if (it == looks_.end()) return nullptr;
    for (const auto& [look, mesh] : it->second.looks)
        if (look == a) return &mesh;
    return nullptr;
}

ActorRenderer::ModelChoice ActorRenderer::choose_model(const game::Spawn& spawn) {
    const std::string_view human = spawn.gender == 1 ? "HUF" : "HUM";
    const auto* rm = game::find_race_model(spawn.race, spawn.gender);
    std::string code(rm ? rm->code : human);
    // A live asset folder renamed many models (Kael's frost giants are FSG there, GAM in RoF2): when the
    // client's code has no model, the live racedata.txt's codes for the race are tried, the spawn's gender first
    // (race 233's male GSM has no model in live; its GSN, for both genders, has).
    if (height_tables_ && !by_code_.contains(code) && !library_->has_model(code)) {
        for (int g : {static_cast<int>(spawn.gender), 2, 0, 1}) {
            std::string_view live = height_tables_->racedata_code(spawn.race, static_cast<std::uint8_t>(g));
            if (!live.empty() && library_->has_model(live)) {
                code = live;
                break;
            }
        }
    }
    if (auto it = by_code_.find(code); it != by_code_.end()) return it->second;
    ModelChoice c;
    c.model = library_->model(code);
    c.actor = c.model || library_->has_model(code);
    if (!c.actor) {
        c.human_fallback = true;
        c.model = library_->model(human);
        c.actor = c.model != nullptr;
    }
    log::debug("race {} gender {}: model {} {}", spawn.race, spawn.gender, code,
               c.human_fallback ? (c.model ? "not loaded, drawn as " + std::string(human) : std::string("not loaded"))
                                : (c.model ? "loaded" : "has nothing to draw"));
    by_code_.emplace(code, c);
    return c;
}

void ActorRenderer::pose_bones(const ActorDraw& a, std::vector<assets::BonePose>& local,
                               std::vector<assets::BonePose>& previous, std::vector<glm::mat4>& world) {
    const auto& m = *a.model;
    // One bone group's tracks (CBoneGroup): the clip, cross-faded from the previous one by the linear weight.
    auto sample = [&](const assets::AnimationClip& clip, float time, bool loop, const assets::AnimationClip* from,
                      float from_time, float blend, std::vector<assets::BonePose>& out) {
        m.sample_local(clip, time, loop, out);
        if (from && blend < 1.0f) {
            m.sample_local(*from, from_time, true, previous);
            float t = std::clamp(blend, 0.0f, 1.0f);
            for (std::size_t b = 0; b < out.size() && b < previous.size(); ++b) {
                out[b].rotation = glm::normalize(glm::slerp(previous[b].rotation, out[b].rotation, t));
                out[b].translation = glm::mix(previous[b].translation, out[b].translation, t);
                out[b].scale = glm::mix(previous[b].scale, out[b].scale, t);
            }
        }
    };
    sample(*a.clip, a.time, a.loop, a.previous_clip, a.previous_time, a.blend, local);
    if (a.upper_clip && a.bone_groups) {
        thread_local std::vector<assets::BonePose> upper;
        sample(*a.upper_clip, a.upper_time, a.upper_loop, a.upper_previous_clip, a.upper_previous_time, a.upper_blend,
               upper);
        const auto& groups = *a.bone_groups;
        for (std::size_t b = 0; b < local.size() && b < upper.size() && b < groups.size(); ++b)
            if (groups[b] == game::kUpperBones) local[b] = upper[b];
    }
    m.compose(local, world);
}

glm::mat4 ActorRenderer::instance_matrix(const ActorDraw& a) {
    if (a.attached) return a.parent; // the mount's bone: its scale, not the rider's own (seat_rider)
    // The actor's turn about +Z as CActorData::SetEulerRotation512
    // @0x100c4200 builds it: the heading (512ths of a turn) through the
    // math object's interpolating sin/cos tables (game/sine_table.h).
    float heading = a.yaw * 256.0f / glm::pi<float>();
    float sh = game::cmath_sin_interp(heading), ch = game::cmath_cos_interp(heading);
    glm::mat4 turn(1.0f);
    turn[0][0] = ch;
    turn[0][1] = sh;
    turn[1][0] = -sh;
    turn[1][1] = ch;
    glm::mat4 instance = glm::translate(glm::mat4(1.0f), a.position) * turn;
    return glm::scale(instance, glm::vec3(a.scale));
}

bool ActorRenderer::spawn_bounds(std::uint32_t id, glm::vec3& origin, float& radius) const {
    auto it = last_draws_.find(id);
    if (it == last_draws_.end() || !it->second.model) return false;
    origin = it->second.position;
    const auto& m = *it->second.model;
    float r = std::max(m.bounding_radius, glm::length(glm::max(glm::abs(m.bounds_min), glm::abs(m.bounds_max))));
    radius = r * it->second.scale;
    return true;
}

const assets::CharacterModel* ActorRenderer::drawn_model(std::uint32_t id) const {
    auto it = last_draws_.find(id);
    return it == last_draws_.end() ? nullptr : it->second.model;
}

float ActorRenderer::drawn_alpha(std::uint32_t id) const {
    auto it = last_draws_.find(id);
    if (it == last_draws_.end() || !it->second.model || !std::isfinite(it->second.alpha)) return 0.0f;
    return std::clamp(it->second.alpha, 0.0f, 1.0f);
}

const assets::CharacterModel* ActorRenderer::spawn_model(std::uint32_t id) const {
    if (const assets::CharacterModel* m = drawn_model(id)) return m;
    auto it = spawns_.find(id);
    return it == spawns_.end() ? nullptr : it->second.model.get();
}

const assets::CharacterMesh* ActorRenderer::drawn_mesh(std::uint32_t id) const {
    auto it = last_draws_.find(id);
    return it == last_draws_.end() ? nullptr : it->second.mesh;
}

bool ActorRenderer::standing(const ActorDraw& a) {
    if (!a.model || !a.clip) return a.model != nullptr;
    return a.clip == a.model->clip("P01", a.holding) || a.clip == a.model->clip("O01", a.holding) ||
           a.clip == a.model->clip("");
}

bool ActorRenderer::drawn_standing(std::uint32_t id) const {
    auto it = last_draws_.find(id);
    return it != last_draws_.end() && standing(it->second);
}

bool ActorRenderer::drawn_playing(std::uint32_t id, std::string_view code, float& time) const {
    auto it = last_draws_.find(id);
    return it != last_draws_.end() && playing(it->second, code, time);
}

bool ActorRenderer::playing(const ActorDraw& a, std::string_view code, float& time) {
    if (!a.model) return false;
    const assets::AnimationClip* clip = a.model->clip(code, a.holding);
    if (!clip) return false;
    // The time into the clip's current play (a looping clip's time runs on past its length).
    auto into = [&](float t, bool loop) {
        const float length = clip->duration_seconds();
        return loop && length > 0.0f ? std::fmod(std::max(t, 0.0f), length) : t;
    };
    if (a.clip == clip) {
        time = into(a.time, a.loop);
        return true;
    }
    if (a.upper_clip == clip) {
        time = into(a.upper_time, a.upper_loop);
        return true;
    }
    return false;
}

bool ActorRenderer::drawn_bone(std::uint32_t id, int bone, glm::mat4& world) const {
    auto it = last_draws_.find(id);
    return it != last_draws_.end() && bone_world(it->second, bone, world);
}

bool ActorRenderer::drawn_transform(std::uint32_t id, glm::mat4& world) const {
    auto it = last_draws_.find(id);
    if (it == last_draws_.end() || !it->second.model) return false;
    world = instance_matrix(it->second);
    return true;
}

bool ActorRenderer::bone_world(const ActorDraw& a, int bone, glm::mat4& world) {
    if (!a.model || !a.clip || bone < 0) return false;
    std::vector<assets::BonePose> local, previous;
    std::vector<glm::mat4> bones;
    pose_bones(a, local, previous, bones);
    if (static_cast<std::size_t>(bone) >= bones.size()) return false;
    world = instance_matrix(a) * bones[static_cast<std::size_t>(bone)];
    return true;
}

bool ActorRenderer::name_position(const ActorDraw& actor, glm::vec3& position) {
    glm::mat4 world;
    if (!actor.model || !bone_world(actor, actor.model->name_bone, world)) return false;
    position = glm::vec3(world[3]);
    return true;
}

bool ActorRenderer::drawn_name_position(std::uint32_t id, glm::vec3& position) const {
    const auto it = last_draws_.find(id);
    return it != last_draws_.end() && name_position(it->second, position);
}

bool ActorRenderer::seat_rider(const ActorDraw& mount, ActorDraw& rider, const game::HeightTables* tables) {
    if (!mount.model || !rider.model) return false;
    int bone = mount.model->mount_point_bone(rider.model->code);
    glm::mat4 at;
    if (bone < 0 || !bone_world(mount, bone, at)) return false;
    // EQGraphicsDX9: the bone's attach (CEQBone vf4 -> the actor's vtable +0x270, CHierarchicalActor @0x10041680,
    // called from UpdateBoneMatrices at 0x10049dec with the bone's acc, bone + 0xa0) copies the bone's world matrix
    // into the rider's model as its world matrix (model vf+0x10 @0x1004a6a0: a plain matrix copy to +0xc) before
    // the rider's bones are composed from it. The rider is drawn in the mount bone's frame, the mount's scale
    // included; its own actor scale and location are not used.
    rider.parent = at;
    // An EQG rider's ROOT_BONE keys are lowered by its moddat.ini ROffset in its model units (the animation loader,
    // 0x1003cd55), so the drop is in the bone's frame too: model_z_offset gives it in the world for a standing actor.
    if (rider.model->eqg)
        rider.parent = at * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -game::eqg_root_drop(tables, rider.model->code, 1.0f)));
    rider.attached = true;
    rider.position = glm::vec3(rider.parent[3]); // bounds and culling
    return true;
}

bool ActorRenderer::rider_top(const ActorDraw& a, glm::vec3& top) {
    if (!a.attached || !a.model) return false;
    top = glm::vec3(a.parent * glm::vec4(0.0f, 0.0f, a.model->bounds_max.z, 1.0f));
    return true;
}

bool ActorRenderer::drawn_rider_top(std::uint32_t id, glm::vec3& top) const {
    auto it = last_draws_.find(id);
    return it != last_draws_.end() && rider_top(it->second, top);
}

bool ActorRenderer::spawn_pose(std::uint32_t id, SpawnPose& out) const {
    out = {};
    auto it = last_draws_.find(id);
    if (it == last_draws_.end() || !it->second.model || !it->second.clip) return false;
    const ActorDraw& a = it->second;
    const auto& m = *a.model;
    const assets::CharacterMesh* mesh = a.mesh;
    if (!mesh) {
        auto d = default_meshes_.find(a.model);
        if (d == default_meshes_.end() || !d->second) return false;
        mesh = d->second.get();
    }
    std::vector<assets::BonePose> local, previous;
    std::vector<glm::mat4> world;
    pose_bones(a, local, previous, world);
    glm::mat4 instance = instance_matrix(a);
    for (std::size_t b = 0; b < world.size() && b < m.parents.size(); ++b) {
        if (m.parents[b] < 0) continue;
        glm::vec3 child(instance * world[b][3]);
        glm::vec3 parent(instance * world[static_cast<std::size_t>(m.parents[b])][3]);
        out.bones.emplace_back(parent, child);
    }
    m.skin(world);
    if (!mesh->append_skin_bones(world)) return false;
    for (auto& w : world) w = instance * w;
    std::vector<glm::vec3> skinned(mesh->vertices.size());
    for (std::size_t v = 0; v < mesh->vertices.size(); ++v) {
        const auto& sv = mesh->vertices[v];
        glm::mat4 bone(0.0f);
        for (int k = 0; k < 4; ++k) {
            if (k > 0 && !(sv.weights[k] > 0.0f)) continue;
            std::size_t b = sv.bones[static_cast<std::size_t>(k)];
            if (b < world.size()) bone += world[b] * sv.weights[k];
        }
        skinned[v] = glm::vec3(bone * glm::vec4(sv.position, 1.0f));
    }
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    out.triangles.reserve(mesh->indices.size());
    for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
        std::uint32_t ia = mesh->indices[i], ib = mesh->indices[i + 1], ic = mesh->indices[i + 2];
        if (ia >= skinned.size() || ib >= skinned.size() || ic >= skinned.size()) continue;
        // EQG models are drawn with the opposite front face (create_pipelines / draw).
        if (m.eqg) std::swap(ib, ic);
        for (auto k : {ia, ib, ic}) {
            out.triangles.push_back(skinned[k]);
            lo = glm::min(lo, skinned[k]);
            hi = glm::max(hi, skinned[k]);
        }
    }
    if (out.triangles.empty()) return false;
    out.center = (lo + hi) * 0.5f;
    for (const auto& p : out.triangles) out.radius = std::max(out.radius, glm::length(p - out.center));
    return true;
}

bool ActorRenderer::spawn_local_triangles(std::uint32_t id, std::vector<glm::vec3>& out) const {
    out.clear();
    auto at = last_spawn_poses_.find(id);
    SpawnPose pose;
    if (at == last_spawn_poses_.end() || !spawn_pose(id, pose)) return false;
    const auto [origin, heading] = at->second;
    const float sh = game::cmath_sin_interp(heading), ch = game::cmath_cos_interp(heading);
    out.reserve(pose.triangles.size());
    for (const auto& p : pose.triangles) {
        const glm::vec3 d = p - origin;
        out.emplace_back(ch * d.x + sh * d.y, -sh * d.x + ch * d.y, d.z);
    }
    return true;
}

bool actor_needs_pose(const Frustum* view, const glm::vec3& center, float radius, bool shadows,
                      const glm::vec3& shadow_from, float shadow_reach) {
    if (!view || box_in_frustum(*view, sphere_box(center, radius))) return true;
    return shadows && (shadow_reach <= 0.0f || glm::length(center - shadow_from) - radius <= shadow_reach);
}

void ActorRenderer::draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                         const game::ViewDistance& view, std::span<const ActorDraw> actors) {
    prepare(frame, actors);
    record(frame, view_proj, camera_pos, view);
}

void ActorRenderer::prepare(const FrameInfo& frame, std::span<const ActorDraw> actors) {
    UploadBatch upload(vk_);
    prepare(frame, actors, upload);
}

void ActorRenderer::prepare(const FrameInfo& frame, std::span<const ActorDraw> actors, UploadBatch& upload) {
    auto t0 = std::chrono::steady_clock::now();
    stats_.instances = 0;
    stats_.draws = 0;
    stats_.unposed = 0;
    prepared_.assign(actors.size(), Prepared{});
    // Upload new models first: that may add textures, and the descriptor
    // sets must be complete before they are bound.
    for (std::size_t i = 0; i < actors.size(); ++i)
        if (actors[i].model && actors[i].clip && std::isfinite(actors[i].alpha) && actors[i].alpha > 0.0f) {
            prepared_[i].alpha = std::min(1.0f, actors[i].alpha);
            prepared_[i].gpu = gpu_model(actors[i].mesh ? *actors[i].mesh : default_mesh(*actors[i].model), upload);
            prepared_[i].eqg = actors[i].model->eqg;
        }
    // One submission (and one fence wait) for all of the frame's new meshes and textures, done before any of this
    // frame's commands that draw them are submitted.
    auto u0 = std::chrono::steady_clock::now();
    upload.submit();
    stats_.upload_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count();
    rebuild_descriptors(frame.frame_slot);
    lighting_buffers_->write(frame.frame_slot, &lighting_, sizeof(lighting_));

    auto* bones = static_cast<glm::mat4*>(bones_mapped_[frame.frame_slot]);
    std::uint32_t used = 0;
    std::vector<assets::BonePose> local, previous;
    std::vector<glm::mat4> world;
    for (std::size_t i = 0; i < actors.size(); ++i) {
        const auto& a = actors[i];
        if (!prepared_[i].gpu) continue;
        const auto& m = *a.model;
        prepared_[i].center = a.position;
        prepared_[i].radius =
            std::max(m.bounding_radius, glm::length(glm::max(glm::abs(m.bounds_min), glm::abs(m.bounds_max)))) * a.scale;
        // Posing (sampling, blending, composing every bone) was most of a busy zone's actor time; one that shows
        // nowhere is not posed, nor drawn.
        if (!actor_needs_pose(pose_view_ ? &*pose_view_ : nullptr, prepared_[i].center, prepared_[i].radius,
                              pose_shadows_, shadow_from_, shadow_reach_)) {
            prepared_[i].gpu = nullptr;
            ++stats_.unposed;
            continue;
        }
        const auto& mesh = a.mesh ? *a.mesh : default_mesh(m);
        if (used + m.parents.size() + mesh.skin_bindings.size() > kMaxBones) {
            prepared_[i].gpu = nullptr;
            continue;
        }
        pose_bones(a, local, previous, world);
        m.skin(world);
        if (!mesh.append_skin_bones(world)) {
            prepared_[i].gpu = nullptr;
            continue;
        }
        glm::mat4 instance = instance_matrix(a);
        prepared_[i].bone_offset = used;
        for (const auto& w : world) bones[used++] = instance * w;
        ++stats_.instances;
    }
    if (used) vmaFlushAllocation(vk_.allocator(), bones_[frame.frame_slot].allocation, 0, sizeof(glm::mat4) * used);
    stats_.cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (stats_.cpu_ms > 8.0)
        log::debug("actor draw took {:.1f} ms ({} meshes, {} textures uploaded so far)", stats_.cpu_ms, models_.size(),
                   textures_.size());
}

void ActorRenderer::draw_shadow(const FrameInfo& frame, std::uint32_t cascade) {
    if (!shadow_pipeline_ || prepared_.empty()) return;
    VkCommandBuffer cmd = frame.cmd;
    VkDescriptorSet sets[2] = {frame_set(frame), vk_.shadow_set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_);
    PushConstants pc{};
    pc.view_proj = lighting_.shadow_view_proj[cascade];
    pc.fog = glm::vec4(kNoFogStart, kNoFogEnd, 0.0f, 1.0f);
    // Only the spawns that can reach the cascade (its clip volume, as the zone's chunks), with the sphere the lights'
    // cubes test them by.
    const Frustum frustum = frustum_from(pc.view_proj);
    for (const auto& p : prepared_) {
        if (!p.gpu || (shadow_reach_ > 0.0f && glm::length(p.center - shadow_from_) - p.radius > shadow_reach_)) continue;
        if (!box_in_frustum(frustum, sphere_box(p.center, p.radius))) continue;
        bool bound = false;
        for (const auto& part : p.gpu->parts) {
            if (part.pass != Pass::Opaque) continue;
            if (!bound) {
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &p.gpu->vertices.buffer, &offset);
                vkCmdBindIndexBuffer(cmd, p.gpu->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                bound = true;
            }
            pc.texture = part.texture;
            pc.bone_offset = p.bone_offset;
            pc.fog.z = part.alpha_ref;
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                               &pc);
            vkCmdDrawIndexed(cmd, part.index_count, 1, part.first_index, 0, 0);
        }
    }
}

bool ActorRenderer::any_in_reach(const glm::vec3& light, float radius) const {
    return std::any_of(prepared_.begin(), prepared_.end(), [&](const Prepared& p) {
        return p.gpu && glm::length(p.center - light) < radius + p.radius;
    });
}

void ActorRenderer::draw_point_shadow(const FrameInfo& frame, const glm::vec3& light, float radius,
                                      std::uint32_t face, std::uint32_t skip_spawn, bool skip_around) {
    if (!point_shadow_pipeline_ || prepared_.empty()) return;
    VkCommandBuffer cmd = frame.cmd;
    VkDescriptorSet sets[2] = {frame_set(frame), vk_.shadow_set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, point_shadow_pipeline_);
    PushConstants pc{};
    pc.fog_color = glm::vec4(light, radius);
    pc.camera_pos = glm::vec4(kPointShadowFaceScale, kPointShadowNear, 0.0f, 0.0f);
    pc.tint = face;
    // prepared_ids_ runs with prepared_ when prepare_spawns posed them (not the viewer's explicit instances).
    const bool ids = prepared_ids_.size() == prepared_.size();
    for (std::size_t i = 0; i < prepared_.size(); ++i) {
        const Prepared& p = prepared_[i];
        float gap = glm::length(p.center - light);
        if (!p.gpu || gap >= radius + p.radius) continue;
        if (shadow_reach_ > 0.0f && glm::length(p.center - shadow_from_) - p.radius > shadow_reach_) continue;
        if (skip_spawn != 0 && ids && prepared_ids_[i] == skip_spawn) continue;
        if (skip_around && gap < p.radius) continue;
        glm::vec3 r(p.radius);
        if (!box_reaches_face(light, face, p.center - r, p.center + r)) continue;
        bool bound = false;
        for (const auto& part : p.gpu->parts) {
            if (part.pass != Pass::Opaque) continue;
            if (!bound) {
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &p.gpu->vertices.buffer, &offset);
                vkCmdBindIndexBuffer(cmd, p.gpu->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                bound = true;
            }
            pc.texture = part.texture;
            pc.bone_offset = p.bone_offset;
            pc.fog.z = part.alpha_ref;
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                               &pc);
            vkCmdDrawIndexed(cmd, part.index_count, 1, part.first_index, 0, 0);
        }
    }
}

void ActorRenderer::record(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                           const game::ViewDistance& view) {
    VkCommandBuffer cmd = frame.cmd;
    PushConstants pc{};
    pc.view_proj = view_proj;
    pc.fog_color = glm::vec4(view.fog_color, view.fog_density); // a: the exp2 fog's density
    pc.camera_pos = glm::vec4(camera_pos, 0.0f);
    bool bound_set = false;
    int bound_pipeline = -1;
    // Only what can show in this view (posed for a shadow is not seen), tested once for the three passes.
    const Frustum frustum = frustum_from(view_proj);
    seen_.assign(prepared_.size(), 0);
    for (std::size_t i = 0; i < prepared_.size(); ++i)
        seen_[i] = prepared_[i].gpu && box_in_frustum(frustum, sphere_box(prepared_[i].center, prepared_[i].radius));
    // The dynamic states set only when they change (unknown on entry: another renderer set them last).
    int cull = -1, front = -1;
    for (int pass = 0; pass < 3; ++pass) {
        for (std::size_t i = 0; i < prepared_.size(); ++i) {
            if (!seen_[i]) continue;
            const Prepared& p = prepared_[i];
            bool any = false;
            for (const auto& part : p.gpu->parts) {
                const Pass draw_pass = p.alpha < 1.0f && part.pass == Pass::Opaque ? Pass::Blended : part.pass;
                if (static_cast<int>(draw_pass) != pass) continue;
                if (!bound_set) {
                    VkDescriptorSet sets[2] = {frame_set(frame), vk_.shadow_set()};
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
                    bound_set = true;
                }
                if (bound_pipeline != pass) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[pass]);
                    bound_pipeline = pass;
                }
                if (!any) {
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &p.gpu->vertices.buffer, &offset);
                    vkCmdBindIndexBuffer(cmd, p.gpu->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                    any = true;
                }
                const VkCullModeFlags want_cull = part.two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
                if (cull != static_cast<int>(want_cull)) {
                    vkCmdSetCullMode(cmd, want_cull);
                    cull = static_cast<int>(want_cull);
                }
                const VkFrontFace want_front = p.eqg ? opposite(kEqFrontFace) : kEqFrontFace;
                if (front != static_cast<int>(want_front)) {
                    vkCmdSetFrontFace(cmd, want_front);
                    front = static_cast<int>(want_front);
                }
                pc.texture = part.texture | part.rough_map << 16;
                pc.tint = part.tint;
                pc.texture2 = (part.texture2 == 0xffffffffu ? 0xffffu : part.texture2) | part.normal_map << 16;
                pc.bone_offset = p.bone_offset;
                pc.fog = part.fog ? glm::vec4(view.fog_start, view.fog_end, part.alpha_ref, part.opacity * p.alpha)
                                  : glm::vec4(kNoFogStart, kNoFogEnd, part.alpha_ref, part.opacity * p.alpha);
                vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(pc), &pc);
                vkCmdDrawIndexed(cmd, part.index_count, 1, part.first_index, 0, 0);
                ++stats_.draws;
            }
        }
    }
}

void ActorRenderer::draw_silhouette(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                                    std::span<const std::uint32_t> ids, const glm::vec4& color) {
    // prepared_ids_ runs with prepared_ when prepare_spawns posed them (not the viewer's explicit instances).
    if (!silhouette_pipeline_ || ids.empty() || prepared_ids_.size() != prepared_.size()) return;
    VkCommandBuffer cmd = frame.cmd;
    PushConstants pc{};
    pc.view_proj = view_proj;
    pc.fog_color = color;
    pc.camera_pos = glm::vec4(camera_pos, 0.0f);
    bool bound = false;
    int front = -1;
    for (std::size_t i = 0; i < prepared_.size(); ++i) {
        const Prepared& p = prepared_[i];
        if (!p.gpu || std::find(ids.begin(), ids.end(), prepared_ids_[i]) == ids.end()) continue;
        if (!bound) {
            VkDescriptorSet sets[2] = {frame_set(frame), vk_.shadow_set()};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, silhouette_pipeline_);
            vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
            bound = true;
        }
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &p.gpu->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, p.gpu->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        const VkFrontFace want_front = p.eqg ? opposite(kEqFrontFace) : kEqFrontFace;
        if (front != static_cast<int>(want_front)) {
            vkCmdSetFrontFace(cmd, want_front);
            front = static_cast<int>(want_front);
        }
        pc.bone_offset = p.bone_offset;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        // The body's opaque parts only: blended and additive ones (hair cards, glows) would outline their quads.
        for (const auto& part : p.gpu->parts) {
            if (part.pass != Pass::Opaque) continue;
            vkCmdDrawIndexed(cmd, part.index_count, 1, part.first_index, 0, 0);
            ++stats_.draws;
        }
    }
}

const std::unordered_set<std::uint32_t>& ActorRenderer::draw_spawns(const FrameInfo& frame, const glm::mat4& view_proj,
                                                                    const glm::vec3& camera_pos,
                                                                    const game::ViewDistance& view,
                                                                    const game::WorldState& world,
                                                                    std::uint32_t hide_id, float dt) {
    prepare_spawns(frame, camera_pos, world, hide_id, dt);
    return draw_prepared(frame, view_proj, camera_pos, view);
}

const std::unordered_set<std::uint32_t>& ActorRenderer::draw_prepared(const FrameInfo& frame,
                                                                      const glm::mat4& view_proj,
                                                                      const glm::vec3& camera_pos,
                                                                      const game::ViewDistance& view) {
    record(frame, view_proj, camera_pos, view);
    drawn_.clear();
    for (auto id : prepared_ids_) drawn_.insert(id);
    for (auto id : empty_ids_) drawn_.insert(id);
    return drawn_;
}

void ActorRenderer::prepare_spawns(const FrameInfo& frame, const glm::vec3& camera_pos,
                                   const game::WorldState& world, std::uint32_t hide_id, float dt) {
    drawn_.clear();
    last_draws_.clear();
    last_spawn_poses_.clear();
    empty_ids_.clear();
    stats_.dressed = 0;
    stats_.undressed = 0;
    stats_.dress_ms = 0.0;
    for (auto& [id, st] : spawns_) st.seen = false;
    height_tables_ = world.height_tables;
    // The frame's new meshes and textures go up in one submission (prepare), not one each.
    UploadBatch upload(vk_);
    // The appearances the worker finished since the last frame (request_dress); applied below within the budget.
    if (dresser_)
        for (auto& [id, d] : dresser_->take()) ready_.insert_or_assign(id, std::move(d));
    core::WorkBudget budget(build_budget_ms_);
    using Ms = std::chrono::duration<double, std::milli>;
    std::vector<ActorDraw> draws;
    std::vector<std::uint32_t> ids;
    dt = std::clamp(dt, 0.0f, 0.25f);
    clock_ += dt;
    now_ms_ = static_cast<std::uint32_t>(clock_ * 1000.0) + 1;
    // Mounts before their riders: a rider is drawn on its mount's pose of this frame.
    std::vector<std::uint32_t> order;
    order.reserve(world.spawns.size());
    for (const auto& [id, s] : world.spawns)
        if (!s.mount_id) order.push_back(id);
    for (const auto& [id, s] : world.spawns)
        if (s.mount_id) order.push_back(id);
    for (std::uint32_t id : order) {
        const game::Spawn& s = world.spawns.at(id);
        // A spawn hidden on purpose (the player in first person, a market-hidden one) keeps its state, and so the
        // look it wears: evict_idle_looks frees only looks no state holds, so it is drawn again in that look at once.
        // Otherwise it is as if it were gone: its dressing is dropped and it comes back as a new state.
        auto keep = [&] {
            auto it = spawns_.find(id);
            if (it == spawns_.end() || !hide_spawn_state(it->second)) return;
            if (dresser_) dresser_->cancel(id);
            ready_.erase(id);
        };
        if (id == hide_id) {
            keep();
            continue;
        }
        if (hidden_ && hidden_->contains(id)) {
            keep();
            empty_ids_.push_back(id);
            continue;
        }
        auto actor = choose_model(s);
        glm::vec3 pos = s.display_valid ? s.display_position : s.position;
        // The camera's actor clip (game::actor_clip_distance): a spawn past it is not drawn, and is not handed to
        // the placeholder boxes either.
        const game::SpawnDraw what =
            game::spawn_draw(actor.actor, actor.model != nullptr, glm::distance(pos, camera_pos), actor_clip_);
        if (what == game::SpawnDraw::Empty || what == game::SpawnDraw::Clipped) empty_ids_.push_back(id);
        if (what != game::SpawnDraw::Model) continue;
        const auto& model = actor.model;
        float heading = s.display_valid ? s.display_heading : s.heading;
        float scale = s.heights.model_scale; // the actor's scale: SetHeights' +0x31c (game/spawn_heights.h)

        auto& st = spawns_[id];
        st.seen = true;
        if (spawn_state_fresh(st, model)) st = SpawnState{model};
        st.seen = true;
        // Appearance: rebuilt when it changes (WearChange, Illusion), on the worker; until it is ready (and applied
        // within the frame's budget) the previous one stays. One applied before for the same model and look is
        // taken at once.
        // Races without per-slot armour show no armour colours (game::drawn_appearance).
        game::Appearance look = game::drawn_appearance(s.appearance, s.race);
        // Only what the active ActorNewArmorFilter makes of the spawn's kind and group/raid membership counts, so
        // a group join does not rebuild a look the filter draws the same.
        const game::AppearanceData* data = library_->appearance_data();
        game::resolve_new_armor_filter(look, data ? data->new_armor_filter : 0u);
        if (!st.dressed || !(st.appearance == look)) {
            st.appearance = look;
            st.dressed = true;
            ready_.erase(id);
            if (const auto* known = applied_look(*model, look)) {
                if (dresser_) dresser_->cancel(id); // a request for an earlier look
                st.mesh = *known;
                if (st.mesh) gpu_model(*st.mesh, upload);
            } else {
                request_dress(id, model, look);
            }
        }
        auto r = ready_.find(id);
        // A texture of it that another job claimed is not decoded here: the appearance waits for that job's
        // result to put the texture up (it is applied in spawn order, so possibly later this frame or the next), at
        // most kMaxDressDefer frames.
        if (r != ready_.end() && budget.may_start() && r->second.deferred < kMaxDressDefer && r->second.plan &&
            waits_for_claimed(*r->second.plan)) {
            ++r->second.deferred;
            r = ready_.end();
        }
        if (r != ready_.end() && budget.may_start()) {
            Dressing d = std::move(r->second);
            ready_.erase(r);
            // The jobs are superseded on every new request, so a result is always for the current one; checked anyway.
            if (d.model == model && d.appearance == st.appearance) {
                ++stats_.dressed;
                auto d0 = std::chrono::steady_clock::now();
                st.mesh = d.mesh;
                const auto& ap = s.appearance;
                log::debug("appearance {} ({}): tex {} helm {} face {} hair {}/{} beard {}/{} materials {} {} {} {} {} {} {} items {} {}",
                           s.name, model->code, ap.texture, ap.helm, ap.face, ap.hair_style, ap.hair_color, ap.beard,
                           ap.beard_color, ap.material[0], ap.material[1], ap.material[2], ap.material[3],
                           ap.material[4], ap.material[5], ap.material[6], ap.material[7], ap.material[8]);
                if (st.mesh) {
                    mesh_refs_.try_emplace(st.mesh.get(), st.mesh);
                    // Its buffers and new textures (decoded on the worker) count against the budget.
                    if (d.plan) install(*st.mesh, *d.plan, upload);
                    else gpu_model(*st.mesh, upload);
                }
                if (!applied_look(*model, d.appearance)) {
                    auto& looks = looks_[model.get()];
                    looks.model = model;
                    looks.looks.emplace_back(d.appearance, d.mesh);
                }
                const double ms = Ms(std::chrono::steady_clock::now() - d0).count();
                budget.done(ms);
                stats_.dress_ms += ms;
            }
        }
        if (ready_.contains(id) || (dresser_ && dresser_->pending(id))) ++stats_.undressed;
        // A spawn still waiting for its appearance is drawn in its model's default one: put up when it is new
        // whatever the budget (it is drawn this frame), and charged to it.
        if (!st.mesh) {
            model_refs_.try_emplace(model.get(), model);
            const auto& fallback = default_mesh(*model);
            if (!models_.contains(&fallback)) {
                auto d0 = std::chrono::steady_clock::now();
                gpu_model(fallback, upload);
                const double ms = Ms(std::chrono::steady_clock::now() - d0).count();
                budget.charge(ms);
                stats_.dress_ms += ms;
            }
        }
        const auto* rm = game::find_race_model(s.race, s.gender);
        const bool new_style = model->eqg && rm && (rm->flags & game::kNewStyleModel);
        // A tunic-point model plays its holding variants while the spawn holds anything (CharacterModel::clip).
        const bool holding = game::holds_item(s.appearance);
        auto has_clip = [&](std::string_view c) { return model->clip(c, holding) != nullptr; };
        auto rand_int = [this] { return static_cast<int>(rng_() & 0x7fff); }; // the C rand()'s range
        const game::Spawn* mount = nullptr;
        if (auto m = s.mount_id ? world.spawns.find(s.mount_id) : world.spawns.end(); m != world.spawns.end())
            mount = &m->second;
        const bool is_player = id == world.player_id;
        const float speed_run = is_player && player_motion_.valid ? player_motion_.speed_run : s.speed_run;
        // PlayerBase::HasProperty(1): the humanoid body type.
        const bool humanoid = s.properties.empty()
                                  ? s.body_type == 1
                                  : std::find(s.properties.begin(), s.properties.end(), 1u) != s.properties.end();
        ActorLayers& layers = st.layers;
        GroupTrack& lower = layers.group[game::kLowerBones];

        // The movement / posture clip as the original chooses it (game/spawn_animation.h).
        game::SpawnAnimInput in;
        in.velocity = is_player && player_motion_.valid ? player_motion_.velocity : s.velocity;
        in.speed_run = speed_run;
        in.mounted = mount != nullptr;
        if (mount && new_style) {
            in.velocity = mount->velocity;
            in.speed_run = mount->speed_run;
        }
        in.feet_wet = is_player && player_motion_.valid ? player_motion_.feet_wet : s.feet_wet;
        in.body_wet = is_player && player_motion_.valid ? player_motion_.body_wet : s.body_wet;
        in.walk_speed = s.walk_speed;
        in.run_speed = s.run_speed;
        in.stand_state = s.stand_state;
        in.npc = s.is_npc;
        in.race = s.race;
        in.gender = s.gender;
        in.new_style = new_style;
        st.new_style = new_style;
        in.new_style_race = rm && (rm->flags & game::kNewStyleModel);
        in.model_scale = scale;
        in.local_player = is_player;
        in.now_ms = now_ms_;
        in.player_state = s.player_state;
        in.speed_heading = is_player && player_motion_.valid ? player_motion_.speed_heading : s.speed_heading;
        in.turning = in.speed_heading != 0.0f;
        in.attacking = is_player && player_motion_.attacking;
        in.owner_id = s.pet_owner_id;
        in.first_person = is_player && player_motion_.first_person;
        if (!new_style) in.follow_up = layers.follow_up;
        in.colliding_type = is_player && player_motion_.valid ? player_motion_.colliding_type : s.colliding_type;
        const bool done = st.motion_clip && lower.clip == st.motion_clip && !lower.loop &&
                          lower.time >= st.motion_clip->duration_seconds();
        game::SpawnAnimChoice choice = game::choose_spawn_animation(
            st.anim, in, has_clip, done, [this] { return 1 + static_cast<int>(rng_() % 100); }, rand_int);
        if (choice.restart && !s.is_npc) // players only: NPCs would flood the log
            log::debug("animation: {} ({}) plays {} (rate {:.3f}{}{})", s.name, id, choice.clip, choice.rate,
                       choice.reverse ? ", reversed" : "", choice.loop ? "" : ", once");
        const assets::AnimationClip* motion = choice.clip.empty() ? nullptr : model->clip(choice.clip, holding);
        if (!motion) motion = &model->first_clip({"P01", "O01", "L01"}, holding);
        st.motion_clip = motion;
        // A stand request that played P08 from +0x10c4 leaves 0x20 or 0x47 there (0x4b15b0's idle path).
        if (!new_style && layers.follow_up == static_cast<std::uint8_t>(AnimationType::StandAfter))
            layers.follow_up = choice.follow_up;

        // What both bone groups play without an action.
        LayerRequest req;
        req.new_style = new_style;
        req.mounted = mount != nullptr;
        req.humanoid = humanoid;
        req.speed_run = speed_run;
        req.follow_up_after = choice.follow_up;
        req.base_column = choice.column;
        if (s.is_corpse || s.stand_state == 115) {
            req.base = &model->first_clip({"D05", "D04", "P01"}, holding);
            req.base_anim = 0x10; // D05
            req.loop = false;
            req.corpse = true;
        } else if (mount && !new_style) {
            // 0x4b15b0 on a rider: its movement and idle animations become the mount race's rider animation
            // (racedata.txt's last field, empty in the shipped file: 0x4a, P09), or 0x4e (L10) while the mount runs
            // at its run speed or faster; a model without the clip falls back as 0x4b1210 does (0x4e to 0x4a,
            // 0x4a to standing). The rider animation is queued on the legs: an action plays on the upper body.
            const bool full_run = mount->run_speed <= mount->speed_run;
            req.base = full_run ? &model->first_clip({"L10", "P09", "P01"}, holding)
                                : &model->first_clip({"P09", "P01"}, holding);
            req.base_anim = full_run ? 0x4e : 0x4a;
        } else {
            req.base = motion;
            req.base_anim = choice.anim;
            req.rate = choice.rate;
            req.loop = choice.loop;
            req.reverse = choice.reverse;
            req.blend_ms = choice.blend_ms;
            req.restart = choice.restart && lower.clip == motion;
        }

        // A new OP_Animation action starts its one-shot clip (game/spawn_animation.h action_clip: a classic model's
        // replacement for a clip it lacks, a new-style model's sequence track), at the packet's speed / 10
        // (HandleWorldMessage's case 0x7177); it becomes the animation playing (+0x10c0), on the upper body alone
        // or on the whole body (render/actor_layers.h).
        if (!st.action_seq_known) {
            st.action_seq = s.action_seq; // an action from before we drew it is not replayed
            st.action_seq_known = true;
        } else if (s.action_seq != st.action_seq) {
            st.action_seq = s.action_seq;
            game::NewStyleTrackInput track_in{s.player_state, st.anim.anim, -1, mount != nullptr};
            std::uint8_t playing = s.action;
            std::string code = game::action_clip(s.action, has_clip, new_style, track_in, rand_int, &playing);
            req.action_column = game::new_style_clip_column(code);
            const assets::AnimationClip* c = code.empty() ? nullptr : model->clip(code, holding);
            const bool started = c && layers.start_action(c, playing, s.action_speed / 10.0f, req);
            log::debug("animation: {} ({}) action {} speed {} -> {}{}", s.name, id, s.action, s.action_speed,
                       code.empty() ? "nothing" : code,
                       started ? (layers.action_on_legs ? "" : " (upper body)") : (c ? " (not played)" : ""));
            if (started) st.anim.anim = playing;
        }
        layers.step(req, dt);
        // Classic models: 0x4b15b0 sets +0x10c0 only for a request it starts, so while the layers keep an
        // action the action is the animation playing that the next stand and idle requests look at
        // (game::classic_plain_stand_after); a walk or run the legs took under it is.
        if (!new_style && layers.action && !layers.legs_took_motion) st.anim.anim = layers.action_anim;

        ActorDraw a;
        a.model = model.get();
        a.mesh = st.mesh.get();
        a.position = pos + glm::vec3(0.0f, 0.0f, model_z_offset(*model, scale, world.height_tables.get()));
        a.yaw = game::heading_to_yaw(heading);
        a.scale = scale;
        a.clip = lower.clip;
        a.holding = holding;
        a.time = lower.shown_time();
        a.loop = lower.loop;
        a.previous_clip = lower.blend < 1.0f ? lower.previous : nullptr;
        a.previous_time = lower.shown_previous_time();
        a.blend = lower.blend;
        if (layers.split()) {
            const GroupTrack& upper = layers.group[game::kUpperBones];
            a.upper_clip = upper.clip;
            a.upper_time = upper.shown_time();
            a.upper_loop = upper.loop;
            a.upper_previous_clip = upper.blend < 1.0f ? upper.previous : nullptr;
            a.upper_previous_time = upper.shown_previous_time();
            a.upper_blend = upper.blend;
            a.bone_groups = &bone_groups(*model);
        }
        if (mount) {
            // Seated on the mount's dag 0xe as it is drawn this frame (a mount without the dag, or not drawn,
            // leaves the rider where its spawn is).
            auto md = last_draws_.find(s.mount_id);
            if (md != last_draws_.end()) seat_rider(md->second, a, world.height_tables.get());
        }
        draws.push_back(a);
        ids.push_back(id);
        last_draws_[id] = a;
        last_spawn_poses_[id] = {pos, heading};
    }
    std::erase_if(spawns_, [this](const auto& kv) {
        if (kv.second.seen) return false;
        // Gone (or past the actor clip, hidden): its appearance, if it comes back, is asked for again.
        if (dresser_) dresser_->cancel(kv.first);
        ready_.erase(kv.first);
        return true;
    });
    evict_idle_looks();
    prepare(frame, draws, upload);
    if (stats_.dressed)
        log::debug("dressed {} spawns in {:.2f} ms, uploaded in {:.2f} ms ({} meshes on the GPU, {} waiting)",
                   stats_.dressed, stats_.dress_ms, stats_.upload_ms, models_.size(), stats_.undressed);
    // Inside "actors prepare_spawns": what of it was new appearances, and their upload.
    if (stats_.dress_ms > 0.0) core::frame_sections::add("actors new appearances", static_cast<float>(stats_.dress_ms));
    if (stats_.upload_ms > 0.0) core::frame_sections::add("actors upload", static_cast<float>(stats_.upload_ms));
    prepared_ids_ = std::move(ids);
}

std::optional<bool> ActorRenderer::jump_animation_playing(std::uint32_t id) const {
    auto it = spawns_.find(id);
    if (it == spawns_.end() || it->second.hidden || !it->second.anim.started) return std::nullopt;
    return game::mechanics::is_jump_animation(it->second.anim.anim, it->second.new_style);
}

const std::vector<std::uint8_t>& ActorRenderer::bone_groups(const assets::CharacterModel& model) {
    auto [it, fresh] = bone_groups_.try_emplace(&model);
    if (fresh) {
        // 0x4090f0 / 0x409260 by the actor's HasNewStyleHierarchicalModel (vtable +0xbc): the EQG skeletons'
        // callback or the classic one, on each bone's tag.
        it->second.reserve(model.bone_names.size());
        for (const auto& name : model.bone_names)
            it->second.push_back(model.eqg ? game::new_style_bone_group(name) : game::classic_bone_group(name));
    }
    return it->second;
}

float ActorRenderer::model_z_offset(const assets::CharacterModel& model, float scale, const game::HeightTables* tables) {
    // The actor's origin is the spawn's z. WLD models are authored with their origin there (ROffset x scale above
    // the feet). EQG models have it at the feet, and the animation loader lowers their ROOT_BONE by moddat.ini's
    // ROffset of the model code (model units): ROffset x scale in the world (game/spawn_heights.h eqg_root_drop).
    return model.eqg ? -game::eqg_root_drop(tables, model.code, scale) : 0.0f;
}

void ActorRenderer::create_pipelines(const std::filesystem::path& shader_dir) {
    VkDevice d = vk_.device();
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout set_layouts[2] = {set_layout_, vk_.shadow_set_layout()};
    li.setLayoutCount = 2;
    li.pSetLayouts = set_layouts;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    vk_check(vkCreatePipelineLayout(d, &li, nullptr, &layout_), "vkCreatePipelineLayout");

    VkShaderModule vs = load_shader(d, shader_dir / "actor.vert.spv");
    VkShaderModule fs = load_shader(d, shader_dir / "actor.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkVertexInputBindingDescription vb{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)},
        {3, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(GpuVertex, bones)},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, weights)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT; // dynamic (two-sided materials, EQG winding)
    rs.frontFace = kEqFrontFace;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = vk_.scene_samples();
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 4;
    ds.pDynamicStates = dyn;
    VkFormat color_format = vk_.scene_format();
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color_format;
    ri.depthAttachmentFormat = vk_.depth_format();

    // Multisampled, the opaque pass turns the alpha test into coverage: alpha-to-coverage, and the fragment
    // shader's constant_id 0 makes its alpha the coverage (shaders/zone.frag, actor.frag, object.frag).
    const VkBool32 coverage = vk_.scene_samples() != VK_SAMPLE_COUNT_1_BIT ? VK_TRUE : VK_FALSE;
    VkSpecializationMapEntry spec_entry{0, 0, sizeof(VkBool32)};
    VkBool32 spec_value = VK_FALSE;
    VkSpecializationInfo spec{1, &spec_entry, sizeof(VkBool32), &spec_value};
    stages[1].pSpecializationInfo = &spec;

    for (int pass = 0; pass < 3; ++pass) {
        spec_value = pass == 0 ? coverage : VK_FALSE;
        ms.alphaToCoverageEnable = spec_value;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = pass == 0 ? VK_TRUE : VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xf;
        if (pass == 1) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (pass == 2) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &ri;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &dss;
        info.pColorBlendState = &cb;
        info.pDynamicState = &ds;
        info.layout = layout_;
        vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &info, nullptr, &pipelines_[pass]),
                 "vkCreateGraphicsPipelines (actors)");
    }

    // The isometric camera's silhouette (draw_silhouette): the same skinning, a flat colour where the actor is behind
    // what is drawn already (reverse-Z: a smaller depth is farther), blended over it (premultiplied), no depth write.
    {
        VkShaderModule xfs = load_shader(d, shader_dir / "actor_silhouette.frag.spv");
        VkPipelineShaderStageCreateInfo xstages[2] = {
            stages[0],
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, xfs, "main",
             nullptr},
        };
        xstages[0].pSpecializationInfo = nullptr;
        VkPipelineMultisampleStateCreateInfo xms = ms;
        xms.alphaToCoverageEnable = VK_FALSE;
        VkPipelineDepthStencilStateCreateInfo xdss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        xdss.depthTestEnable = VK_TRUE;
        xdss.depthWriteEnable = VK_FALSE;
        xdss.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState xba{};
        xba.colorWriteMask = 0xf;
        xba.blendEnable = VK_TRUE;
        xba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        xba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        xba.colorBlendOp = VK_BLEND_OP_ADD;
        xba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        xba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        xba.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo xcb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        xcb.attachmentCount = 1;
        xcb.pAttachments = &xba;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &ri;
        info.stageCount = 2;
        info.pStages = xstages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &xms;
        info.pDepthStencilState = &xdss;
        info.pColorBlendState = &xcb;
        info.pDynamicState = &ds;
        info.layout = layout_;
        vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &info, nullptr, &silhouette_pipeline_),
                 "vkCreateGraphicsPipelines (actor silhouette)");
        vkDestroyShaderModule(d, xfs, nullptr);
    }

    // Shadow casters (draw_shadow): skinned with the cascade as view_proj, depth only, no culling.
    VkShaderModule svs = load_shader(d, shader_dir / "actor_shadow.vert.spv");
    VkShaderModule sfs = load_shader(d, shader_dir / "actor_shadow.frag.spv");
    VkPipelineShaderStageCreateInfo shadow_stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, svs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sfs, "main", nullptr},
    };
    VkPipelineRasterizationStateCreateInfo srs = rs;
    srs.cullMode = VK_CULL_MODE_NONE;
    srs.depthBiasEnable = VK_TRUE;
    VkPipelineMultisampleStateCreateInfo sms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    sms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo sdss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    sdss.depthTestEnable = VK_TRUE;
    sdss.depthWriteEnable = VK_TRUE;
    sdss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo scb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState sdyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo sds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    sds.dynamicStateCount = 3;
    sds.pDynamicStates = sdyn;
    VkPipelineRenderingCreateInfo sri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    sri.depthAttachmentFormat = vk_.shadow_format();
    VkGraphicsPipelineCreateInfo sinfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    sinfo.pNext = &sri;
    sinfo.stageCount = 2;
    sinfo.pStages = shadow_stages;
    VkVertexInputAttributeDescription shadow_attrs[] = {attrs[0], attrs[2], attrs[3], attrs[4]}; // no normal
    VkPipelineVertexInputStateCreateInfo svi = vi;
    svi.vertexAttributeDescriptionCount = 4;
    svi.pVertexAttributeDescriptions = shadow_attrs;
    sinfo.pVertexInputState = &svi;
    sinfo.pInputAssemblyState = &ia;
    sinfo.pViewportState = &vp;
    sinfo.pRasterizationState = &srs;
    sinfo.pMultisampleState = &sms;
    sinfo.pDepthStencilState = &sdss;
    sinfo.pColorBlendState = &scb;
    sinfo.pDynamicState = &sds;
    sinfo.layout = layout_;
    vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &sinfo, nullptr, &shadow_pipeline_),
             "vkCreateGraphicsPipelines (actor shadows)");
    vkDestroyShaderModule(d, svs, nullptr);
    vkDestroyShaderModule(d, sfs, nullptr);

    // The lights' shadow cubes (draw_point_shadow): the same, writing the distance, with no bias.
    VkShaderModule pvs = load_shader(d, shader_dir / "actor_point_shadow.vert.spv");
    VkShaderModule pfs = load_shader(d, shader_dir / "actor_point_shadow.frag.spv");
    shadow_stages[0].module = pvs;
    shadow_stages[1].module = pfs;
    srs.depthBiasEnable = VK_FALSE;
    sds.dynamicStateCount = 2;
    vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &sinfo, nullptr, &point_shadow_pipeline_),
             "vkCreateGraphicsPipelines (actor point shadows)");
    vkDestroyShaderModule(d, pvs, nullptr);
    vkDestroyShaderModule(d, pfs, nullptr);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
}

} // namespace hailstone::render
