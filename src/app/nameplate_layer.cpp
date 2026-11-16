#include "app/nameplate_layer.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>

#include <imgui_impl_vulkan.h>

#include "core/log.h"
#include "render/vk_util.h"

namespace hailstone::app {

NameplateLayer::NameplateLayer(render::VkContext& vk, const std::filesystem::path& shader_dir)
    : vk_(vk), device_(vk.device()), allocator_(vk.allocator()) {
    try {
        create(shader_dir);
        usable_ = true;
    } catch (const std::exception& e) {
        log::warn("name plates: no depth test ({}); names are drawn over the scene", e.what());
    }
}

void NameplateLayer::create(const std::filesystem::path& shader_dir) {
    using render::vk_check;
    const render::VkContext& vk = vk_;
    // Set 0 must be defined as ImGui's is (one combined image sampler, fragment stage) and the push constants as its
    // own (the vertex stage's scale and translate), or the font texture its draw commands bind would not fit our
    // pipeline. Set 1 is ours: the scene's depth, and a dynamic uniform buffer with each plate's parameters.
    VkDescriptorSetLayoutBinding font{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &font;
    vk_check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &font_layout_), "vkCreateDescriptorSetLayout (plate font)");
    VkDescriptorSetLayoutBinding ours[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    li.bindingCount = 2;
    li.pBindings = ours;
    vk_check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layout_), "vkCreateDescriptorSetLayout (plate depth)");

    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4};
    VkDescriptorSetLayout sets[2] = {font_layout_, set_layout_};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2;
    pl.pSetLayouts = sets;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    vk_check(vkCreatePipelineLayout(device_, &pl, nullptr, &layout_), "vkCreatePipelineLayout (plate depth)");

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.physical_device(), &props);
    const VkDeviceSize align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1);
    stride_ = (sizeof(Params) + align - 1) / align * align;

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, render::VkContext::kFramesInFlight},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, render::VkContext::kFramesInFlight}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = render::VkContext::kFramesInFlight;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    vk_check(vkCreateDescriptorPool(device_, &pi, nullptr, &pool_), "vkCreateDescriptorPool (plate depth)");
    std::array<VkDescriptorSetLayout, render::VkContext::kFramesInFlight> layouts;
    layouts.fill(set_layout_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = render::VkContext::kFramesInFlight;
    ai.pSetLayouts = layouts.data();
    vk_check(vkAllocateDescriptorSets(device_, &ai, sets_.data()), "vkAllocateDescriptorSets (plate depth)");
    for (std::uint32_t s = 0; s < render::VkContext::kFramesInFlight; ++s) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = stride_ * kMaxPlates;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        vk_check(vmaCreateBuffer(allocator_, &bi, &ac, &buffers_[s], &allocations_[s], &info), "vmaCreateBuffer (plate depth)");
        mapped_[s] = info.pMappedData;
        VkDescriptorBufferInfo buffer{buffers_[s], 0, sizeof(Params)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets_[s];
        w.dstBinding = 1;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    // The pipeline: ImGui's (its vertices, blending, dynamic viewport and scissor, the swapchain's format, no depth
    // attachment) with our fragment shader.
    VkShaderModule vert = render::create_shader_module(device_, shader_dir / "plate_depth.vert.spv");
    VkShaderModule frag = render::create_shader_module(device_, shader_dir / "plate_depth.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr}};
    VkVertexInputBindingDescription binding{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(ImDrawVert, pos))},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(ImDrawVert, uv))},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<std::uint32_t>(offsetof(ImDrawVert, col))}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;
    const VkFormat format = vk.color_format();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo pc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pc.pNext = &rendering;
    pc.stageCount = 2;
    pc.pStages = stages;
    pc.pVertexInputState = &vi;
    pc.pInputAssemblyState = &ia;
    pc.pViewportState = &vp;
    pc.pRasterizationState = &rs;
    pc.pMultisampleState = &ms;
    pc.pColorBlendState = &cb;
    pc.pDynamicState = &dyn;
    pc.layout = layout_;
    const VkResult r = vkCreateGraphicsPipelines(device_, vk.pipeline_cache(), 1, &pc, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    vk_check(r, "vkCreateGraphicsPipelines (plate depth)");
}

NameplateLayer::~NameplateLayer() {
    vk_.wait_idle();
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (font_layout_) vkDestroyDescriptorSetLayout(device_, font_layout_, nullptr);
    for (std::uint32_t s = 0; s < render::VkContext::kFramesInFlight; ++s)
        if (buffers_[s]) vmaDestroyBuffer(allocator_, buffers_[s], allocations_[s]);
}

void NameplateLayer::begin(const render::FrameInfo& frame, ImDrawList* dl, const glm::vec4& view, float near_plane,
                           float far_plane) {
    slot_ = frame.frame_slot;
    dl_ = dl;
    plates_ = 0;
    if (!usable_) return;
    const ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
    view_ = glm::vec4(view.x * fb.x, view.y * fb.y, view.z * fb.x, view.w * fb.y);
    // view depth = y / (d + x), reverse Z (FlyCamera::projection)
    depth_terms_ = {near_plane / (far_plane - near_plane), far_plane * near_plane / (far_plane - near_plane)};
    // The scene's depth, as the post pass reads it: the frame's own, valid from begin_overlay (which the UI's render
    // calls before it runs ImGui's draw commands) to the end of the frame.
    VkDescriptorImageInfo image{vk_.post_sampler(), vk_.overlay_depth_view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets_[slot_];
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

void NameplateLayer::bind(const ImDrawList*, const ImDrawCmd* cmd) {
    const auto* data = static_cast<const Callback*>(cmd->UserCallbackData);
    const auto* rs = static_cast<const ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    NameplateLayer& l = *data->layer;
    vkCmdBindPipeline(rs->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, l.pipeline_);
    const std::uint32_t offset = static_cast<std::uint32_t>(data->index * l.stride_);
    vkCmdBindDescriptorSets(rs->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, l.layout_, 1, 1, &l.sets_[l.slot_], 1, &offset);
}

bool NameplateLayer::draw(const NameplateText& plate) {
    if (plate.canvas && (!supports_canvas() || plate.canvas->hide)) return false;
    if (usable_ && depth_test_ && !plate.through_walls && plates_ >= kMaxPlates - (plate.target ? 0u : 1u)) return false;
    if (!usable_ || !depth_test_ || plate.through_walls) {
        return paint(plate);
    }
    Params params{view_, glm::vec4(depth_terms_.x, depth_terms_.y, plate.depth, 3.0f), glm::vec4(occluded_alpha_, 0.0f, 0.0f, 0.0f)};
    std::memcpy(static_cast<char*>(mapped_[slot_]) + plates_ * stride_, &params, sizeof(params));
    vmaFlushAllocation(allocator_, allocations_[slot_], plates_ * stride_, sizeof(params));
    Callback data{this, plates_++};
    dl_->AddCallback(&NameplateLayer::bind, &data, sizeof(data)); // ImGui keeps a copy of the data
    const bool painted = paint(plate);
    dl_->AddCallback(ImDrawCallback_ResetRenderState, nullptr);   // and ImGui's own pipeline again
    return painted;
}

ImFont* NameplateLayer::font() const { return style_.font ? style_.font : ImGui::GetFont(); }

float NameplateLayer::name_size(const NameplateText& p) const {
    const float base = style_.size > 0.0f ? style_.size : font()->FontSize;
    return base * std::max(p.scale, 0.01f);
}

glm::vec2 NameplateLayer::size_of(const NameplateText& p) const {
    if (p.canvas) return {p.canvas->bounds.w, p.canvas->bounds.h};
    ImFont* f = font();
    const float size = name_size(p), tag = size * style_.tag_scale;
    const float bold = style_.faux_bold ? 1.0f : 0.0f;
    float w = f->CalcTextSizeA(size, FLT_MAX, 0.0f, p.name.c_str()).x + bold;
    for (const auto& line : p.lines) w = std::max(w, f->CalcTextSizeA(tag, FLT_MAX, 0.0f, line.c_str()).x + bold);
    return {w, size + tag * static_cast<float>(p.lines.size())};
}

glm::vec4 NameplateLayer::bounds_of(const NameplateText& p) const {
    if (p.canvas) return nameplate_canvas_bounds(*p.canvas, p.pos);
    const auto size = size_of(p);
    return {p.pos.x - size.x * 0.5f, p.pos.y, p.pos.x + size.x * 0.5f, p.pos.y + size.y};
}

bool NameplateLayer::paint(const NameplateText& p) const {
    if (p.canvas) {
        auto style = style_.canvas;
        if (!style.font) style.font = font();
        return paint_nameplate_canvas(*dl_, *p.canvas, p.pos, style, p.alpha);
    }
    const float px = p.pos.x, py = p.pos.y;
    // Every colour times the plate's alpha.
    auto fade = [&](ImU32 c) {
        const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(p.alpha, 0.0f, 1.0f);
        return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
    };
    ImFont* f = font();
    const float fs = name_size(p), tag = fs * style_.tag_scale;
    const ImU32 color = fade(p.color);
    // The edge: a drop shadow one pixel down and right (the look before the options), or a dark outline around each
    // letter, thicker for a larger name.
    const ImU32 edge = fade(style_.edge == game::PlateEdge::Outline ? IM_COL32(0, 0, 0, 220) : IM_COL32(0, 0, 0, 200));
    const float ring = std::max(1.0f, std::round(fs / 16.0f));
    auto text = [&](ImVec2 at, float size, ImU32 c, const char* s) {
        dl_->AddText(f, size, at, c, s);
        if (style_.faux_bold) dl_->AddText(f, size, ImVec2(at.x + 1.0f, at.y), c, s);
    };
    auto line = [&](float y, float size, const std::string& s) {
        const float w = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str()).x + (style_.faux_bold ? 1.0f : 0.0f);
        const ImVec2 at(px - w * 0.5f, y);
        if (style_.edge == game::PlateEdge::Shadow) {
            text(ImVec2(at.x + 1, at.y + 1), size, edge, s.c_str());
        } else if (style_.edge == game::PlateEdge::Outline) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy)
                        text(ImVec2(at.x + static_cast<float>(dx) * ring, at.y + static_cast<float>(dy) * ring), size,
                             edge, s.c_str());
        }
        text(at, size, color, s.c_str());
    };
    if (p.highlight) {
        // The target: its colour on a dark plate edged in that colour, with a marker bobbing above it (Hailstone's
        // look, not the original's; its ring: ClientApp::draw_target_ring).
        const ImU32 shadow = fade(IM_COL32(0, 0, 0, 200));
        const float w = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, p.name.c_str()).x;
        const float padx = fs * 0.45f, pady = fs * 0.12f;
        ImVec2 a(px - w * 0.5f - padx, py - pady), b(px + w * 0.5f + padx, py + fs + pady);
        dl_->AddRectFilled(a, b, fade(IM_COL32(0, 0, 0, 150)), fs * 0.35f);
        dl_->AddRect(a, b, color, fs * 0.35f, 0, std::max(1.5f, fs * 0.1f));
        const float bob = std::abs(std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f)) * fs * 0.3f;
        const float tip = a.y - fs * 0.25f - bob, half = fs * 0.4f;
        ImVec2 t0(px - half, tip - half * 1.2f), t1(px + half, tip - half * 1.2f), t2(px, tip);
        dl_->AddTriangleFilled(ImVec2(t0.x, t0.y + 1.5f), ImVec2(t1.x, t1.y + 1.5f), ImVec2(t2.x, t2.y + 1.5f), shadow);
        dl_->AddTriangleFilled(t0, t1, t2, color);
    }
    line(py, fs, p.name);
    float y = py + fs;
    for (const auto& l : p.lines) {
        line(y, tag, l);
        y += tag;
    }
    return true;
}

} // namespace hailstone::app
