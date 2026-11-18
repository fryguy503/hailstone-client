#include "render/sky_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/sky.h"
#include "assets/texture.h"
#include "assets/texture_overrides.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "render/cloud_renderer.h"
#include "render/vk_util.h"

namespace hailstone::render {

namespace {

struct GpuVertex {
    glm::vec3 position;
    glm::vec2 uv;
    std::uint32_t entry; // sky colour table entry, 0..17
};

// Matches the push_constant block in sky.vert / sky.frag.
struct PushConstants {
    glm::mat4 view_proj;
    glm::vec4 tint;
    glm::vec2 uv_offset;
    std::uint32_t texture;
    std::uint32_t use_table;
    glm::vec4 haze; // SkyFrame::horizon_haze; a 0 for the sun and moon
    glm::vec4 light{0.0f}; // a > 0: the colour (linear) is scaled by rgb (SkyFrame::cloud_light); a 2..3: keyed
                           // clouds, the fraction the storm
};

// shaders/atmosphere.vert / .frag's push constants, in the same range.
struct AtmospherePush {
    glm::mat4 inverse_view_proj; // clip to the sky's directions (view rotation only)
    glm::vec4 to_sun;            // w: haze in the air (1 normal, more for the desert)
    glm::vec4 params;            // x: seconds, y: the stars' turn (radians, with the sun's path), z: storm, w: flash
    glm::vec4 haze;              // SkyFrame::horizon_haze, a: 0 off, else 1 + SkyFrame::aurora
    glm::vec4 to_moon;           // SkyFrame::to_moon, w: SkyFrame::blood_moon
};
static_assert(sizeof(AtmospherePush) <= sizeof(PushConstants));
static_assert(sizeof(PushConstants) <= 128);

struct ColorBlock {
    glm::vec4 colors[assets::kSkyColors];
};


VkShaderModule load_shader(VkDevice device, const std::filesystem::path& path) {
    return create_shader_module(device, path);
}

} // namespace

float outdoor_brightness(float hour) {
    return 0.3f + 0.7f * assets::sky_light_level(assets::sky_level(hour));
}

SkyRenderer::SkyRenderer(VkContext& vk, const assets::Sky& sky, const std::filesystem::path& shader_dir,
                         const GameDir* game, std::string_view zone)
    : vk_(vk), index_(sky.index), shader_dir_(shader_dir) {
    std::vector<GpuVertex> vertices;
    std::vector<std::uint32_t> indices;

    UploadBatch upload(vk_);
    {
        assets::Image white;
        white.width = white.height = 1;
        white.mips.push_back(std::vector<std::byte>(4, std::byte{0xff}));
        textures_.push_back(upload.texture(white));
        // No volumetric clouds: nothing of their light, all of the sky behind (shaders/atmosphere.frag).
        assets::Image clear;
        clear.width = clear.height = 1;
        clear.mips.push_back({std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xff}});
        clear_clouds_ = upload.texture(clear);
    }
    std::unordered_map<std::string, std::uint32_t> slots;
    auto slot_for = [&](const assets::Material& m) -> std::uint32_t {
        if (m.frames.empty()) return 0;
        const std::string& name = m.frames.front();
        bool masked = m.blend == assets::BlendMode::Masked;
        std::string key = name + (masked ? "#m" : "");
        if (auto it = slots.find(key); it != slots.end()) return it->second;
        std::uint32_t slot = 0;
        std::string error;
        std::optional<assets::Image> image;
        image = assets::decode_game_texture(name, sky.read_file(name),
                                            {.palette_index0_transparent = masked, .wld_bmp_rows = true,
                                             .reads_alpha = m.blend != assets::BlendMode::Opaque},
                                            &error);
        if (image && textures_.size() < kMaxTextures) {
            slot = static_cast<std::uint32_t>(textures_.size());
            textures_.push_back(upload.texture(*image, TextureUse::Color));
        } else {
            log::warn("sky texture {}: {}", name, image ? "too many textures" : error);
        }
        slots.emplace(std::move(key), slot);
        return slot;
    };

    auto add = [&](const assets::Mesh& mesh, const std::vector<std::uint8_t>* entries, Kind kind) {
        auto base = static_cast<std::uint32_t>(vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& v = mesh.vertices[i];
            std::uint32_t entry = entries && i < entries->size() ? (*entries)[i] : 16u;
            vertices.push_back({v.position, v.uv, entry});
        }
        for (const auto& part : mesh.parts) {
            if (part.material >= mesh.materials.size()) continue;
            const auto& m = mesh.materials[part.material];
            if (m.blend == assets::BlendMode::Invisible) continue;
            Draw d;
            d.kind = kind;
            d.first_index = static_cast<std::uint32_t>(indices.size());
            for (std::uint32_t i = 0; i < part.index_count; ++i) {
                std::uint32_t index = mesh.indices[part.first_index + i];
                if (index < mesh.vertices.size()) indices.push_back(base + index);
            }
            d.index_count = static_cast<std::uint32_t>(indices.size()) - d.first_index;
            d.texture = slot_for(m);
            // RoF2 itself draws the procedural CSky and never loads sky.s3d. For these
            // classic layers the DLL's legacy render-method table
            // (CRender::InitLegacyRenderMethodTable @0x1009aa40, read by
            // CMaterial::ResolveTransparency @0x100138d0) gives 0x8000000d opaque and
            // 0x8000000f translucent at alpha 0x80. Other methods keep the layer rule:
            // the sun and moon add light, clouds blend by their method.
            switch (m.raw_render_method) {
            case 0x8000000du: d.pipeline = 0; break;
            case 0x8000000fu: d.pipeline = 1; d.opacity = 128.0f / 255.0f; break;
            default:
                switch (kind) {
                case Kind::Dome: d.pipeline = 0; break;
                case Kind::Bodies: d.pipeline = 2; break;
                case Kind::Clouds:
                    switch (m.blend) {
                    case assets::BlendMode::Additive: d.pipeline = 2; break;
                    case assets::BlendMode::Transparent25: d.pipeline = 1; d.opacity = 0.25f; break;
                    case assets::BlendMode::Transparent75: d.pipeline = 1; d.opacity = 0.75f; break;
                    default: d.pipeline = 1; d.opacity = 0.5f; break;
                    }
                    break;
                }
                break;
            }
            log::debug("sky {} layer {}: render method {:#x} -> pipeline {}", sky.index, static_cast<int>(kind),
                       m.raw_render_method, d.pipeline);
            if (d.index_count) draws_.push_back(d);
        }
    };
    if (sky.dome) add(sky.dome->mesh, &sky.dome->color_entry, Kind::Dome);
    if (sky.bodies) add(*sky.bodies, nullptr, Kind::Bodies);
    if (sky.clouds) add(sky.clouds->mesh, &sky.clouds->color_entry, Kind::Clouds);

    // The procedural sky's cloud patches (Resources/Sky/Cloud-<Texture>.dds).
    if (game) {
        std::uint32_t seed = 1; // MSVC rand(): seed * 214013 + 2531011, bits 16..30
        auto rand = [seed]() mutable {
            seed = seed * 214013u + 2531011u;
            return static_cast<int>((seed >> 16) & 0x7fff);
        };
        for (auto& def : assets::load_zone_clouds(*game, zone)) {
            std::uint32_t slot = 0;
            std::string file = "Resources/Sky/Cloud-" + def.texture + ".dds";
            if (auto data = game->read(file))
                if (auto image = assets::decode_texture(*data); image && textures_.size() < kMaxTextures) {
                    slot = static_cast<std::uint32_t>(textures_.size());
                    textures_.push_back(upload.texture(*image, TextureUse::Color));
                }
            if (slot == 0) {
                log::warn("sky cloud texture {} not loaded", file);
                continue;
            }
            Clouds c{game::CloudLayer(def, rand), slot};
            c.layer.fill();
            clouds_.push_back(std::move(c));
        }
        if (!clouds_.empty()) log::info("sky: {} cloud layers for {}", clouds_.size(), zone);
    }

    if (vertices.empty() || indices.empty()) throw std::runtime_error("sky has no drawable layers");
    vertex_buffer_ = upload.buffer(std::as_bytes(std::span(vertices)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    index_buffer_ = upload.buffer(std::as_bytes(std::span(indices)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    upload.submit();

    for (std::size_t i = 0; i < colors_.size(); ++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(ColorBlock);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        vk_check(vmaCreateBuffer(vk_.allocator(), &bi, &ai, &colors_[i].buffer, &colors_[i].allocation, &out),
                 "create sky colour buffer");
        colors_[i].size = bi.size;
        colors_mapped_[i] = out.pMappedData;
    }

    for (std::size_t i = 0; i < cloud_vertices_.size() && !clouds_.empty(); ++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(GpuVertex) * kMaxCloudVertices;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        vk_check(vmaCreateBuffer(vk_.allocator(), &bi, &ai, &cloud_vertices_[i].buffer, &cloud_vertices_[i].allocation,
                                 &out),
                 "create cloud vertex buffer");
        cloud_vertices_[i].size = bi.size;
        cloud_mapped_[i] = out.pMappedData;
    }

    create_descriptors();
    create_pipelines(shader_dir);
    log::info("sky {}: {} draws, {} triangles, {} textures", index_, draws_.size(), indices.size() / 3,
              textures_.size());
}

SkyRenderer::~SkyRenderer() {
    VkDevice d = vk_.device();
    vk_.wait_idle();
    for (VkPipeline p : pipelines_)
        if (p) vkDestroyPipeline(d, p, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
    volumetric_.reset();
    for (auto& t : textures_) t.destroy(d, vk_.allocator());
    clear_clouds_.destroy(d, vk_.allocator());
    for (auto& b : colors_) b.destroy(vk_.allocator());
    for (auto& b : cloud_vertices_) b.destroy(vk_.allocator());
    vertex_buffer_.destroy(vk_.allocator());
    index_buffer_.destroy(vk_.allocator());
}

void SkyRenderer::create_descriptors() {
    VkDevice d = vk_.device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    vk_check(vkCreateSampler(d, &si, nullptr, &sampler_), "vkCreateSampler");

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = kMaxTextures;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    // The volumetric clouds (shaders/atmosphere.frag), written each frame (draw).
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = bindings;
    vk_check(vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");

    constexpr auto kSets = static_cast<std::uint32_t>(VkContext::kFramesInFlight);
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (kMaxTextures + 1) * kSets},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kSets;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    vk_check(vkCreateDescriptorPool(d, &pi, nullptr, &pool_), "vkCreateDescriptorPool");

    std::array<VkDescriptorSetLayout, VkContext::kFramesInFlight> layouts;
    layouts.fill(set_layout_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSets;
    ai.pSetLayouts = layouts.data();
    vk_check(vkAllocateDescriptorSets(d, &ai, sets_.data()), "vkAllocateDescriptorSets");

    std::vector<VkDescriptorImageInfo> images;
    for (std::uint32_t i = 0; i < kMaxTextures; ++i) {
        const Texture& t = textures_[i < textures_.size() ? i : 0];
        images.push_back({sampler_, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    for (std::uint32_t s = 0; s < kSets; ++s) {
        VkDescriptorBufferInfo buffer{colors_[s].buffer, 0, sizeof(ColorBlock)};
        VkDescriptorImageInfo clear{sampler_, clear_clouds_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[3]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = sets_[s];
        w[0].dstBinding = 0;
        w[0].descriptorCount = kMaxTextures;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = images.data();
        w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[1].dstSet = sets_[s];
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[1].pBufferInfo = &buffer;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[2].dstSet = sets_[s];
        w[2].dstBinding = 2;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &clear;
        vkUpdateDescriptorSets(d, 3, w, 0, nullptr);
    }
}

void SkyRenderer::create_pipelines(const std::filesystem::path& shader_dir) {
    VkDevice d = vk_.device();
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &set_layout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    vk_check(vkCreatePipelineLayout(d, &li, nullptr, &layout_), "vkCreatePipelineLayout");

    VkShaderModule vs = load_shader(d, shader_dir / "sky.vert.spv");
    VkShaderModule fs = load_shader(d, shader_dir / "sky.frag.spv");
    VkShaderModule avs = load_shader(d, shader_dir / "atmosphere.vert.spv");
    VkShaderModule afs = load_shader(d, shader_dir / "atmosphere.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo no_vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkVertexInputBindingDescription vb{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)},
        {2, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, entry)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // the original's sky pass disables culling
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = vk_.scene_samples();
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = vk_.scene_rendering();
    // Reverse Z: every sky vertex is pinned to the clear depth (0). Test each sample,
    // without writing it, so the whole sky stack can follow opaque geometry.
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_FALSE;
    dss.depthCompareOp = VK_COMPARE_OP_EQUAL;

    for (int pass = 0; pass < 4; ++pass) {
        if (pass == 3) {
            stages[0].module = avs;
            stages[1].module = afs;
        }
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT;
        if (pass == 1) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (pass == 2) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        const SceneBlends blends = vk_.scene_blends(ba, SceneOutput::Nothing);
        const VkPipelineColorBlendStateCreateInfo cb = blends.info();

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &ri;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = pass == 3 ? &no_vertices : &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &dss;
        info.pColorBlendState = &cb;
        info.pDynamicState = &ds;
        info.layout = layout_;
        vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &info, nullptr, &pipelines_[pass]),
                 "vkCreateGraphicsPipelines");
    }
    for (VkShaderModule m : {vs, fs, avs, afs}) vkDestroyShaderModule(d, m, nullptr);
}

void SkyRenderer::prepare(const FrameInfo& frame, const glm::mat4& projection, const glm::mat4& view,
                          const SkyFrame& sky) {
    volumetric_ready_ = false;
    if (!(sky.atmosphere && sky.volumetric_clouds && has_atmosphere()) || volumetric_failed_) return;
    if (!volumetric_) {
        try {
            volumetric_ = std::make_unique<CloudRenderer>(vk_, shader_dir_);
        } catch (const std::exception& e) {
            // A device that cannot make them (a missing shader, an allocation): the painted clouds, as before.
            log::warn("volumetric clouds: {}", e.what());
            volumetric_failed_ = true;
            return;
        }
    }
    CloudFrame c;
    c.sky_view_proj = projection * glm::mat4(glm::mat3(view));
    c.cover = cloud_cover(index_, sky.storm);
    c.to_light = sky.to_light;
    c.light = sky.light;
    c.ambient = sky.sky_light;
    c.storm = sky.storm;
    c.seconds = sky.real_seconds;
    volumetric_->record(frame, c);
    volumetric_ready_ = true;
}

void SkyRenderer::draw(const FrameInfo& frame, const glm::mat4& projection, const glm::mat4& view,
                       const SkyFrame& sky) {
    if (draws_.empty()) return;
    // The volumetric clouds this frame, or none (the clear image): the frame slot's set is not in use now.
    const bool volumetric = volumetric_ready_ && sky.atmosphere && has_atmosphere();
    {
        VkDescriptorImageInfo image = volumetric
            ? VkDescriptorImageInfo{volumetric_->sampler(), volumetric_->view(), VK_IMAGE_LAYOUT_GENERAL}
            : VkDescriptorImageInfo{sampler_, clear_clouds_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets_[frame.frame_slot];
        w.dstBinding = 2;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &image;
        vkUpdateDescriptorSets(vk_.device(), 1, &w, 0, nullptr);
    }
    volumetric_ready_ = false;
    float level = assets::sky_level(sky.hour);
    ColorBlock block{};
    for (int e = 0; e < assets::kSkyColors; ++e) block.colors[e] = glm::vec4(assets::sky_color(level, e), 1.0f);
    std::memcpy(colors_mapped_[frame.frame_slot], &block, sizeof(block));
    vmaFlushAllocation(vk_.allocator(), colors_[frame.frame_slot].allocation, 0, VK_WHOLE_SIZE);

    VkCommandBuffer cmd = frame.cmd;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[frame.frame_slot], 0, nullptr);

    glm::mat4 rotation = glm::mat4(glm::mat3(view)); // camera at the centre of the sky
    glm::mat4 sky_vp = projection * rotation;
    // The sun and moon ride the day-cycle sweep (assets::sky_sweep_heading):
    // a turn is 512 and heading 0 is noon. The skeleton's rest pose has the
    // moon overhead and the sun below, so the rest pose is midnight (heading
    // 256). They turn about the north-south axis (world +X is north, heading
    // 0) so that the sun rises in the east (-Y) at 6:00 and sets in the west
    // at 18:00. The original's orientation is not traced (docs/unknowns.md).
    float heading = assets::sky_sweep_heading(sky.hour) + 256.0f;
    glm::mat4 bodies = glm::rotate(glm::mat4(1.0f), -heading * 2.0f * std::numbers::pi_v<float> / 512.0f,
                                   glm::vec3(1.0f, 0.0f, 0.0f));

    // The modern mode's atmosphere in place of the dome.
    const bool atmosphere = sky.atmosphere && has_atmosphere();
    if (atmosphere) {
        AtmospherePush ap{};
        ap.inverse_view_proj = glm::inverse(sky_vp);
        ap.to_sun = glm::vec4(sky.to_sun, index_ == 2 ? 2.5f : 1.0f);
        ap.params = glm::vec4(static_cast<float>(std::fmod(sky.real_seconds, 1024.0)),
                              -heading * 2.0f * std::numbers::pi_v<float> / 512.0f, sky.storm, sky.flash);
        ap.haze = sky.horizon_haze;
        if (ap.haze.a > 0.0f) ap.haze.a = 1.0f + std::clamp(sky.aurora, 0.0f, 1.0f); // shaders/atmosphere.frag
        ap.to_moon = glm::vec4(sky.to_moon, sky.blood_moon);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[3]);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ap), &ap);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    int bound = -1;
    // The dome mirrored under the horizon first, for a zone floating in the sky: the real one is drawn over it.
    if (sky.mirror_below && !atmosphere) {
        const glm::mat4 mirrored = sky_vp * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));
        for (const auto& d : draws_) {
            if (d.kind == Kind::Bodies) continue;
            if (d.pipeline != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[d.pipeline]);
                bound = d.pipeline;
            }
            PushConstants pc{};
            pc.view_proj = mirrored;
            pc.tint = glm::vec4(glm::vec3(1.0f), d.opacity);
            pc.texture = d.texture;
            pc.use_table = 1u;
            pc.haze = sky.horizon_haze;
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
        }
    }
    for (const auto& d : draws_) {
        // The atmosphere has its own sun and moon (shaders/atmosphere.frag) in place of the dome and the sprites.
        if (atmosphere && (d.kind == Kind::Dome || d.kind == Kind::Bodies)) continue;
        // The volumetric clouds stand in for the painted layer.
        if (volumetric && d.kind == Kind::Clouds) continue;
        // Over the atmosphere the cloud layer blends, its clouds keyed out of its painted sky.
        int pipeline = atmosphere && d.kind == Kind::Clouds ? 1 : d.pipeline;
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[pipeline]);
            bound = pipeline;
        }
        PushConstants pc{};
        pc.view_proj = d.kind == Kind::Bodies ? sky_vp * bodies : sky_vp;
        // The modern mode (haze given) lights the sun and moon past white, so they bloom.
        float glow = d.kind == Kind::Bodies && sky.horizon_haze.a > 0.0f ? 4.0f : 1.0f;
        pc.tint = glm::vec4(glm::vec3(glow), d.opacity);
        pc.texture = d.texture;
        pc.use_table = d.kind == Kind::Bodies ? 0u : 1u;
        if (atmosphere && d.kind == Kind::Clouds) {
            pc.use_table = 0u;
            pc.tint.a = 1.0f;
            pc.light = glm::vec4(sky.cloud_light, 2.0f + std::clamp(sky.storm, 0.0f, 0.99f));
        }
        if (d.kind != Kind::Bodies) pc.haze = sky.horizon_haze;
        // No UV scroll: RoF2's sky layers do not scroll (its clouds are the
        // moving patches below).
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
    }
    // ...and for the weather.ini patches; their motion is still kept up, so they are in place if the option goes off.
    draw_clouds(cmd, frame.frame_slot, sky_vp, sky, atmosphere, volumetric);
}

void SkyRenderer::draw_clouds(VkCommandBuffer cmd, std::uint32_t slot, const glm::mat4& sky_vp, const SkyFrame& sky,
                              bool atmosphere, bool hidden) {
    if (clouds_.empty()) return;
    // dt_ms = the tick delta x TimeScale 1.0; 1 on the first frame and when 0.
    double dt = last_seconds_ < 0.0 ? 0.001 : sky.real_seconds - last_seconds_;
    last_seconds_ = sky.real_seconds;
    float dt_ms = static_cast<float>(dt * 1000.0) * time_scale_;
    if (dt_ms <= 0.0f) dt_ms = 1.0f;
    for (auto& c : clouds_) c.layer.update(dt_ms);
    if (hidden) return;

    // Geometry (not traced; docs/unknowns.md): a patch is a grid of
    // SubdivisionX x SubdivisionY quads on the unit sky sphere, centred where
    // the zenith turned by -angle about +Y points, moved off the orbit by its
    // Offset (radians about the orbit's tangent), Size the half extents in
    // radians, Rotation spinning it in place.
    auto* out = static_cast<GpuVertex*>(cloud_mapped_[slot]);
    std::uint32_t count = 0;
    struct Batch { std::uint32_t first, count, texture; float opacity; };
    std::vector<Batch> batches;
    for (const auto& c : clouds_) {
        const auto& def = c.layer.definition();
        int sx = std::clamp(def.subdivision_x, 1, 16), sy = std::clamp(def.subdivision_y, 1, 16);
        for (const auto& p : c.layer.patches()) {
            std::uint32_t need = static_cast<std::uint32_t>(sx * sy * 6);
            if (count + need > kMaxCloudVertices) break;
            glm::mat4 orbit = glm::rotate(glm::mat4(1.0f), -p.angle, glm::vec3(0, 1, 0));
            glm::vec3 centre = glm::vec3(orbit * glm::vec4(0, 0, 1, 0));
            glm::vec3 tangent = glm::vec3(orbit * glm::vec4(1, 0, 0, 0));
            glm::vec3 side(0, 1, 0);
            centre = glm::normalize(centre * std::cos(p.offset) + side * std::sin(p.offset));
            side = glm::normalize(glm::cross(centre, tangent));
            float cr = std::cos(p.rotation), sr = std::sin(p.rotation);
            glm::vec3 ax = tangent * cr + side * sr, ay = -tangent * sr + side * cr;
            auto at = [&](int i, int j) {
                float u = static_cast<float>(i) / static_cast<float>(sx), v = static_cast<float>(j) / static_cast<float>(sy);
                glm::vec3 d = centre + ax * ((u * 2.0f - 1.0f) * p.size_x) + ay * ((v * 2.0f - 1.0f) * p.size_y);
                return GpuVertex{glm::normalize(d), glm::vec2(u, v), 16u};
            };
            std::uint32_t first = count;
            for (int j = 0; j < sy; ++j)
                for (int i = 0; i < sx; ++i)
                    for (auto [di, dj] : {std::pair{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}) out[count++] = at(i + di, j + dj);
            batches.push_back({first, count - first, c.texture, game::CloudLayer::opacity(p)});
        }
    }
    if (batches.empty()) return;
    vmaFlushAllocation(vk_.allocator(), cloud_vertices_[slot].allocation, 0, VK_WHOLE_SIZE);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &cloud_vertices_[slot].buffer, &offset);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[1]);
    // The cloud colour sets (weather.ini ColorSet -> colour maps) are not
    // decoded: white, dimmed with the outdoor light.
    float light = outdoor_brightness(sky.hour);
    for (const auto& b : batches) {
        PushConstants pc{};
        pc.view_proj = sky_vp;
        pc.tint = glm::vec4(glm::vec3(atmosphere ? 1.0f : light), b.opacity);
        pc.texture = b.texture;
        pc.use_table = 0;
        pc.haze = sky.horizon_haze;
        if (atmosphere) pc.light = glm::vec4(sky.cloud_light, 1.0f);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, b.count, 1, b.first, 0);
    }
}

} // namespace hailstone::render
