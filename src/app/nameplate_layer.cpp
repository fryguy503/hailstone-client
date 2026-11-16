#include "app/nameplate_layer.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>

#include <imgui_impl_vulkan.h>
#include <imgui_internal.h>

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
    // A replacement must never silently lose its depth test. Ordinary captions retain the historical fallback;
    // the app checks camera-to-head collision before using that fallback for a failed replacement.
    if (plate.presentation && !supports_presentations()) return false;
    // The target draws last, so reserve its slot even in a crowded scene.
    if (usable_ && depth_test_ && !plate.through_walls && plates_ >= kMaxPlates - (plate.target ? 0u : 1u)) return false;
    if (!usable_ || !depth_test_ || plate.through_walls) {
        paint(plate);
        return true;
    }
    Params params{view_, glm::vec4(depth_terms_.x, depth_terms_.y, plate.depth, 3.0f), glm::vec4(occluded_alpha_, 0.0f, 0.0f, 0.0f)};
    std::memcpy(static_cast<char*>(mapped_[slot_]) + plates_ * stride_, &params, sizeof(params));
    vmaFlushAllocation(allocator_, allocations_[slot_], plates_ * stride_, sizeof(params));
    Callback data{this, plates_++};
    dl_->AddCallback(&NameplateLayer::bind, &data, sizeof(data)); // ImGui keeps a copy of the data
    paint(plate);
    dl_->AddCallback(ImDrawCallback_ResetRenderState, nullptr);   // and ImGui's own pipeline again
    return true;
}

namespace {

ImFont* plate_font(const NameplateDrawStyle& style) { return style.font ? style.font : ImGui::GetFont(); }

float name_size(const NameplateText& p, const NameplateDrawStyle& style) {
    const float base = style.size > 0.0f ? style.size : plate_font(style)->FontSize;
    return base * std::max(p.scale, 0.01f);
}

float text_width(ImFont* font, float size, const std::string& text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
}

std::string fitted_text(ImFont* font, float size, const std::string& text, float width) {
    // Chip edges are placed in window pixels; subtracting their large coordinates can lose a fraction of a pixel.
    // Do not ellipsize a label that fits before that rounding.
    if (text_width(font, size, text) <= width + 0.25f) return text;
    const float dots = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "...").x;
    if (dots >= width) return {};
    const char* end = text.c_str();
    font->CalcTextSizeA(size, width - dots, 0.0f, text.c_str(), nullptr, &end);
    return std::string(text.c_str(), end) + "...";
}

glm::vec4 text_ink_overhang(ImFont* font, float size, const std::string& text) {
    glm::vec4 overhang{0.0f}; // left, above, right, below the nominal advance/line box
    const float scale = size / font->FontSize;
    const char* current = text.data();
    const char* end = current + text.size();
    while (current < end) {
        unsigned int codepoint = 0;
        const int consumed = ImTextCharFromUtf8(&codepoint, current, end);
        if (consumed <= 0) break;
        current += consumed;
        if (const auto* glyph = font->FindGlyph(static_cast<ImWchar>(codepoint))) {
            overhang.x = std::max(overhang.x, -glyph->X0 * scale);
            overhang.y = std::max(overhang.y, -glyph->Y0 * scale);
            overhang.z = std::max(overhang.z, (glyph->X1 - glyph->AdvanceX) * scale);
            overhang.w = std::max(overhang.w, (glyph->Y1 - font->FontSize) * scale);
        }
    }
    return overhang;
}

struct PresentationLayout {
    struct Badge { float x = 0.0f, y = 0.0f, width = 0.0f; };
    float unit = 1.0f, width = 0.0f, height = 0.0f, font = 0.0f, tag = 0.0f, gap = 0.0f;
    float margin_x = 0.0f, margin_y = 0.0f, total_width = 0.0f, cast_width = 0.0f, frame_outset = 0.0f;
    float body_width = 0.0f, side_extra = 0.0f;
    float marker = 0.0f, badge_size = 0.0f, badge_font = 0.0f, badge_gap = 0.0f, name_y = 0.0f, subtitle_y = 0.0f;
    float health_y = 0.0f, mana_y = 0.0f, cast_y = 0.0f;
    float health_h = 0.0f, mana_h = 0.0f, cast_h = 0.0f;
    std::array<Badge, 8> badges{};
    std::size_t badge_count = 0;
};

PresentationLayout presentation_layout(const NameplateText& p, const NameplateDrawStyle& style) {
    const auto& a = *p.presentation;
    ImFont* font = plate_font(style);
    PresentationLayout l;
    l.unit = std::max(p.scale * a.scale, 0.01f);
    l.width = a.width * l.unit;
    l.cast_width = (a.cast_width > 0.0f ? std::max(24.0f, a.cast_width) : a.width) * l.unit;
    const float body_width = a.cast ? std::max(l.width, l.cast_width) : l.width;
    l.body_width = body_width;
    l.frame_outset = (a.frame_style == "bevel" || a.frame_style == "glass" ? 3.0f : a.frame_style == "runic" ? 4.0f : 0.0f) * l.unit;
    float decoration = a.frame_style == "runic" ? 17.0f * l.unit : l.frame_outset + 2.0f * l.unit;
    if (p.target && a.show_marker) {
        if (a.target_style == "chevrons") decoration = std::max(decoration, 20.0f * l.unit);
        if (a.target_style == "brackets") decoration = std::max(decoration, 6.0f * l.unit);
        if (a.target_style == "glow") decoration = std::max(decoration, 9.0f * l.unit);
    }
    l.margin_x = std::max(2.0f, decoration);
    l.margin_y = std::max(2.0f, l.frame_outset + 2.0f * l.unit);
    if (a.frame_style == "runic") l.margin_y = std::max(l.margin_y, 9.0f * l.unit);
    if (p.target && a.show_marker && a.target_style == "chevrons") l.margin_y = std::max(l.margin_y, 10.0f * l.unit);
    if (p.target && a.show_marker && a.target_style == "glow") l.margin_y = std::max(l.margin_y, 9.0f * l.unit);
    if (p.target && a.show_marker && a.target_style == "brackets") l.margin_y = std::max(l.margin_y, 6.0f * l.unit);
    l.font = name_size(p, style) * a.scale * a.font_scale;
    l.tag = l.font * 0.8f;
    l.gap = 2.0f * l.unit;
    l.marker = p.target && a.show_marker && a.target_style == "arrow" ? 10.0f * l.unit : 0.0f;
    l.badge_gap = l.gap * a.badge_scale;
    l.badge_font = l.tag * a.badge_scale;
    const bool side = a.badge_position == "left" || a.badge_position == "right";
    const float badge_limit = side ? std::min(l.width, 140.0f * l.unit) : l.width;
    l.badge_size = std::min(std::max(14.0f * l.unit, l.tag) * a.badge_scale,
                            std::max(1.0f, badge_limit - l.badge_gap * 2.0f));
    glm::vec4 ink = text_ink_overhang(font, l.font, a.name ? *a.name : p.name);
    const auto include_ink = [&](float size, const std::string& text) {
        ink = glm::max(ink, text_ink_overhang(font, size, text));
    };
    if (a.subtitle) include_ink(l.tag, *a.subtitle);
    for (const auto* bar : {&a.health, &a.mana, &a.cast}) if (*bar) include_ink(l.font * 0.72f, (*bar)->text);
    for (const auto& badge : a.badges) include_ink(l.badge_font, badge.label);
    // Font glyphs can descend beyond FontSize (notably the built-in face scaled up). Include actual ink plus
    // its shadow instead of assuming every text row's nominal line height contains every glyph.
    l.margin_x = std::max(l.margin_x, std::ceil(std::max(ink.x, ink.z) + l.unit));
    l.margin_y = std::max(l.margin_y, std::ceil(std::max(ink.y, ink.w) + l.unit));
    decoration = std::max(decoration, l.margin_x);
    l.badge_count = std::min(l.badges.size(), a.badges.size());
    float column_width = 0.0f;
    for (std::size_t i = 0; i < l.badge_count; ++i) {
        const auto& badge = a.badges[i];
        const float limit = side ? std::min(l.width, 140.0f * l.unit) : l.width;
        const float label = badge.label.empty() ? 0.0f : std::min(text_width(font, l.badge_font, badge.label),
                                                                  std::max(0.0f, limit - l.badge_size - l.badge_gap * 3.0f));
        l.badges[i].width = std::min(limit, l.badge_size + l.badge_gap * 2.0f + (label > 0.0f ? label + l.badge_gap : 0.0f));
        column_width = std::max(column_width, l.badges[i].width);
    }
    l.total_width = body_width + l.margin_x * 2.0f;
    if (side && l.badge_count > 0) {
        l.side_extra = column_width + l.badge_gap + decoration;
        l.total_width += l.side_extra * 2.0f;
    }
    float y = l.marker;
    const auto badge_rows = [&](float top) {
        float x = 0.0f, row_y = top;
        for (std::size_t i = 0; i < l.badge_count; ++i) {
            auto& badge = l.badges[i];
            if (x > 0.0f && x + badge.width > l.width) { x = 0.0f; row_y += l.badge_size + l.badge_gap; }
            badge.x = x; badge.y = row_y;
            x += badge.width + l.badge_gap;
        }
        return l.badge_count > 0 ? row_y + l.badge_size + l.badge_gap : top;
    };
    if (!side && a.badge_position == "above") y = badge_rows(y);
    const float body_top = y;
    const float row_gap = l.gap + l.frame_outset * 2.0f;
    const bool inside = a.name_position == "inside" && a.health.has_value();
    l.name_y = y;
    if (a.show_name && a.name_position != "below" && !inside) y += l.font + row_gap;
    l.subtitle_y = y;
    if (a.subtitle && !a.subtitle->empty()) y += l.tag + row_gap;
    const float bar_font = l.font * 0.72f;
    auto bar_height = [&](const std::optional<game::NameplateBar>& bar, float height) {
        return bar ? std::max(height * l.unit, bar->text.empty() ? 0.0f : bar_font + l.gap) : 0.0f;
    };
    l.health_h = bar_height(a.health, a.height);
    if (a.show_name && inside) l.health_h = std::max(l.health_h, l.font + l.gap);
    l.mana_h = bar_height(a.mana, a.mana_height);
    l.cast_h = bar_height(a.cast, a.cast_height);
    l.health_y = y;
    if (a.health) y += l.health_h;
    if (a.health && a.mana) y += row_gap;
    l.mana_y = y;
    if (a.mana) y += l.mana_h;
    if (a.cast && (a.health || a.mana)) y += a.cast_gap * l.unit + l.frame_outset * 2.0f;
    l.cast_y = y;
    if (a.cast) y += l.cast_h;
    if (a.show_name && a.name_position == "below") {
        if (a.health || a.mana || a.cast) y += row_gap;
        l.name_y = y;
        y += l.font;
    }
    if (side && l.badge_count > 0) {
        const float column_h = static_cast<float>(l.badge_count) * (l.badge_size + l.badge_gap) - l.badge_gap;
        const float body_h = y - body_top;
        const float shift = std::max(0.0f, (column_h - body_h) * 0.5f);
        l.name_y += shift; l.subtitle_y += shift; l.health_y += shift; l.mana_y += shift; l.cast_y += shift;
        const float column_top = body_top + std::max(0.0f, (body_h - column_h) * 0.5f);
        for (std::size_t i = 0; i < l.badge_count; ++i) {
            auto& badge = l.badges[i];
            badge.x = a.badge_position == "left" ? -(body_width - l.width) * 0.5f - decoration - l.badge_gap - column_width :
                                                  (body_width + l.width) * 0.5f + decoration + l.badge_gap;
            badge.y = column_top + static_cast<float>(i) * (l.badge_size + l.badge_gap);
        }
        y = body_top + std::max(body_h, column_h);
    } else if (a.badge_position == "below" && l.badge_count > 0) {
        y += row_gap;
        y = badge_rows(y);
    }
    l.height = y;
    return l;
}

ImU32 rgb(std::uint32_t color, float alpha) {
    return IM_COL32((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff,
                    static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
}

ImU32 tone(ImU32 color, float factor) {
    const auto channel = [&](int shift) {
        return static_cast<ImU32>(std::clamp(static_cast<float>((color >> shift) & 0xff) * factor, 0.0f, 255.0f));
    };
    return (color & IM_COL32_A_MASK) | (channel(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) |
           (channel(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) | (channel(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT);
}

// These simple vector symbols are authored here. They use the font's white texel and therefore share the exact
// same depth-tested pipeline as text and bars; no game textures or server-specific artwork are needed.
void paint_badge_icon(ImDrawList& dl, const std::string& icon, ImVec2 c, float r, ImU32 color, ImU32 background) {
    const float stroke = std::max(1.0f, r * 0.18f);
    if (icon == "eye") {
        dl.AddBezierQuadratic(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y - r * 1.2f), ImVec2(c.x + r, c.y), color, stroke);
        dl.AddBezierQuadratic(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y + r * 1.2f), ImVec2(c.x + r, c.y), color, stroke);
        dl.AddCircleFilled(c, r * 0.3f, color, 12);
    } else if (icon == "peace") {
        dl.AddTriangleFilled(ImVec2(c.x, c.y + r * 0.2f), ImVec2(c.x - r * 0.7f, c.y - r),
                             ImVec2(c.x + r * 0.15f, c.y - r * 0.3f), color);
        dl.AddTriangleFilled(ImVec2(c.x - r * 0.3f, c.y + r * 0.3f), ImVec2(c.x - r, c.y + r * 0.6f),
                             ImVec2(c.x - r * 0.6f, c.y - r * 0.2f), color);
        dl.AddCircleFilled(ImVec2(c.x + r * 0.15f, c.y + r * 0.1f), r * 0.45f, color, 12);
        dl.AddCircleFilled(ImVec2(c.x + r * 0.55f, c.y - r * 0.2f), r * 0.25f, color, 12);
        dl.AddTriangleFilled(ImVec2(c.x + r * 0.7f, c.y - r * 0.3f), ImVec2(c.x + r, c.y - r * 0.15f),
                             ImVec2(c.x + r * 0.7f, c.y), color);
        dl.AddLine(ImVec2(c.x + r * 0.6f, c.y + r * 0.15f), ImVec2(c.x + r * 0.9f, c.y + r * 0.8f), color, stroke);
    } else if (icon == "sleep") {
        const float a = r * 0.8f;
        dl.AddLine(ImVec2(c.x - a, c.y - a), ImVec2(c.x + a, c.y - a), color, stroke);
        dl.AddLine(ImVec2(c.x + a, c.y - a), ImVec2(c.x - a, c.y + a), color, stroke);
        dl.AddLine(ImVec2(c.x - a, c.y + a), ImVec2(c.x + a, c.y + a), color, stroke);
    } else if (icon == "star") {
        std::array<ImVec2, 10> points;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const float a = static_cast<float>(i) * 0.62831853f - 1.57079633f;
            const float radius = i % 2 == 0 ? r : r * 0.43f;
            points[i] = ImVec2(c.x + std::cos(a) * radius, c.y + std::sin(a) * radius);
        }
        for (std::size_t i = 0; i < points.size(); ++i) dl.AddTriangleFilled(c, points[i], points[(i + 1) % points.size()], color);
    } else if (icon == "sun") {
        dl.AddCircleFilled(c, r * 0.5f, color, 16);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.78539816f;
            const ImVec2 v(std::cos(a), std::sin(a));
            dl.AddLine(ImVec2(c.x + v.x * r * 0.7f, c.y + v.y * r * 0.7f), ImVec2(c.x + v.x * r, c.y + v.y * r), color, stroke);
        }
    } else if (icon == "moon") {
        dl.AddCircleFilled(c, r * 0.9f, color, 20);
        dl.AddCircleFilled(ImVec2(c.x + r * 0.4f, c.y - r * 0.25f), r * 0.75f, background, 20);
    } else if (icon == "diamond") {
        dl.AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.8f, c.y), ImVec2(c.x, c.y + r),
                         ImVec2(c.x - r * 0.8f, c.y), color);
    } else if (icon == "triangle") {
        dl.AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y + r * 0.8f), ImVec2(c.x - r, c.y + r * 0.8f), color);
    } else if (icon == "cross") {
        dl.AddRectFilled(ImVec2(c.x - r * 0.25f, c.y - r), ImVec2(c.x + r * 0.25f, c.y + r), color);
        dl.AddRectFilled(ImVec2(c.x - r, c.y - r * 0.25f), ImVec2(c.x + r, c.y + r * 0.25f), color);
    } else if (icon == "skull") {
        dl.AddCircleFilled(ImVec2(c.x, c.y - r * 0.2f), r * 0.8f, color, 16);
        dl.AddRectFilled(ImVec2(c.x - r * 0.5f, c.y), ImVec2(c.x + r * 0.5f, c.y + r * 0.9f), color);
        dl.AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.2f), r * 0.2f, background, 8);
        dl.AddCircleFilled(ImVec2(c.x + r * 0.3f, c.y - r * 0.2f), r * 0.2f, background, 8);
        dl.AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x, c.y + r * 0.9f), background, stroke);
    } else if (icon == "sword") {
        dl.AddQuadFilled(ImVec2(c.x - r * 0.15f, c.y + r * 0.2f), ImVec2(c.x + r * 0.75f, c.y - r * 0.85f),
                         ImVec2(c.x + r * 0.85f, c.y - r * 0.75f), ImVec2(c.x + r * 0.1f, c.y + r * 0.4f), color);
        dl.AddLine(ImVec2(c.x - r * 0.65f, c.y + r * 0.75f), ImVec2(c.x - r * 0.1f, c.y + r * 0.2f), color, stroke * 2.0f);
        dl.AddLine(ImVec2(c.x - r * 0.5f, c.y), ImVec2(c.x + r * 0.4f, c.y + r * 0.7f), color, stroke * 1.5f);
    } else {
        dl.AddCircle(c, r * 0.8f, color, 16, stroke);
    }
}

void paint_presentation(ImDrawList& dl, const NameplateText& p, const NameplateDrawStyle& style) {
    const auto& a = *p.presentation;
    if (a.hide) return;
    const auto l = presentation_layout(p, style);
    ImFont* font = plate_font(style);
    const float alpha = p.alpha * a.alpha;
    const ImU32 text_color = rgb(a.text_color, alpha), border = rgb(a.border_color, alpha);
    const ImU32 background = rgb(a.background_color, alpha);
    const ImU32 shadow = rgb(0, alpha * 0.82f);
    const float left = p.pos.x - l.width * 0.5f, top_y = p.pos.y + l.margin_y;
    const bool inside = a.name_position == "inside" && a.health.has_value();
    const bool target = p.target && a.show_marker && a.target_style != "none";
    auto text = [&](float y, float size, const std::string& label, float width, float x, const std::string& align) {
        const std::string fit = fitted_text(font, size, label, width);
        const float w = text_width(font, size, fit);
        const float offset = align == "left" ? 0.0f : align == "right" ? width - w : (width - w) * 0.5f;
        const ImVec2 at(x + offset, top_y + y);
        dl.AddText(font, size, ImVec2(at.x + l.unit, at.y + l.unit), shadow, fit.c_str());
        dl.AddText(font, size, at, text_color, fit.c_str());
    };
    if (l.marker > 0.0f) {
        const float r = l.marker * 0.35f;
        dl.AddTriangleFilled(ImVec2(p.pos.x - r, top_y + l.unit), ImVec2(p.pos.x + r, top_y + l.unit),
                             ImVec2(p.pos.x, top_y + r * 1.8f), border);
    }
    for (std::size_t i = 0; i < l.badge_count; ++i) {
        const auto& badge = a.badges[i];
        const auto& pos = l.badges[i];
        const ImVec2 top(left + pos.x, top_y + pos.y), bottom(top.x + pos.width, top.y + l.badge_size);
        if (a.frame_style != "minimal") dl.AddRectFilled(top, bottom, background, l.unit * 2.0f);
        if (a.frame_style == "bevel" || a.frame_style == "runic") dl.AddRect(top, bottom, border, l.unit, 0, l.unit);
        const ImVec2 center(top.x + l.badge_gap + l.badge_size * 0.5f, top.y + l.badge_size * 0.5f);
        bool image = false;
        if (badge.spell_icon && style.spell_icon) {
            if (auto icon = style.spell_icon(*badge.spell_icon); icon && icon->texture != ImTextureID{}) {
                const float radius = l.badge_size * 0.45f;
                dl.AddImage(icon->texture, ImVec2(center.x - radius, center.y - radius),
                            ImVec2(center.x + radius, center.y + radius), icon->uv0, icon->uv1, rgb(0xffffff, alpha));
                image = true;
            }
        }
        if (!image) paint_badge_icon(dl, badge.icon, center, l.badge_size * 0.35f, rgb(badge.color, alpha), background);
        const float label_left = top.x + l.badge_size + l.badge_gap * 2.0f;
        if (!badge.label.empty() && label_left < bottom.x - l.badge_gap) {
            const std::string fit = fitted_text(font, l.badge_font, badge.label, bottom.x - l.badge_gap - label_left);
            dl.AddText(font, l.badge_font, ImVec2(label_left, top.y + (l.badge_size - l.badge_font) * 0.5f),
                       rgb(badge.color, alpha), fit.c_str());
        }
    }
    if (a.show_name && !inside) text(l.name_y, l.font, a.name ? *a.name : p.name, l.width, left, a.text_align);
    if (a.subtitle && !a.subtitle->empty()) text(l.subtitle_y, l.tag, *a.subtitle, l.width, left, a.text_align);
    const auto shaded_fill = [&](ImVec2 top, ImVec2 bottom, ImU32 color, float rounding) {
        if (bottom.x <= top.x || bottom.y <= top.y) return;
        const int start = dl.VtxBuffer.Size;
        dl.AddRectFilled(top, bottom, color, rounding);
        if (a.texture == "gradient" || a.texture == "gloss") {
            for (int i = start; i < dl.VtxBuffer.Size; ++i) {
                auto& vertex = dl.VtxBuffer[i];
                const float amount = std::clamp((vertex.pos.y - top.y) / (bottom.y - top.y), 0.0f, 1.0f);
                vertex.col = tone(vertex.col, 1.15f - amount * 0.4f);
            }
        }
        if (a.texture == "gloss") {
            const float shine = (bottom.y - top.y) * 0.42f;
            dl.AddRectFilled(top, ImVec2(bottom.x, top.y + shine), rgb(0xffffff, alpha * 0.23f), std::min(rounding, shine * 0.5f));
            dl.AddLine(ImVec2(top.x, top.y + shine), ImVec2(bottom.x, top.y + shine), rgb(0xffffff, alpha * 0.18f),
                       std::min(l.unit, bottom.y - top.y));
        }
    };
    auto bar = [&](const std::optional<game::NameplateBar>& value, float y, float height, float width,
                   const std::string& align, bool primary, bool health) {
        if (!value) return;
        const float x = p.pos.x - width * 0.5f;
        const ImVec2 top(x, top_y + y), bottom(x + width, top_y + y + height);
        const float edge = std::min({a.border_width * l.unit, height * 0.5f, width * 0.5f});
        const float rounding = std::min(a.rounding * l.unit, height * 0.5f);
        const float out = l.frame_outset;
        if (a.frame_style == "bevel") {
            const ImVec2 outer(top.x - out, top.y - out), end(bottom.x + out, bottom.y + out);
            dl.AddRectFilled(outer, end, tone(border, 0.45f));
            dl.AddLine(outer, ImVec2(end.x, outer.y), tone(border, 1.85f), l.unit * 1.5f);
            dl.AddLine(outer, ImVec2(outer.x, end.y), tone(border, 1.4f), l.unit * 1.5f);
            dl.AddLine(ImVec2(outer.x, end.y), end, rgb(0, alpha), l.unit * 1.5f);
            dl.AddLine(ImVec2(end.x, outer.y), end, rgb(0, alpha), l.unit * 1.5f);
        } else if (a.frame_style == "glass") {
            const float radius = std::max(rounding, height * 0.35f);
            dl.AddRectFilled(ImVec2(top.x - out, top.y - out), ImVec2(bottom.x + out, bottom.y + out), rgb(0x060b13, alpha * 0.88f), radius);
            dl.AddRect(ImVec2(top.x - out, top.y - out), ImVec2(bottom.x + out, bottom.y + out), rgb(0xb4d1e8, alpha * 0.32f), radius, 0, l.unit);
        } else if (a.frame_style == "runic") {
            const float cut = std::min(height * 0.5f + out, 7.0f * l.unit);
            const std::array<ImVec2, 6> points{{{top.x - out + cut, top.y - out}, {bottom.x + out - cut, top.y - out},
                {bottom.x + out, (top.y + bottom.y) * 0.5f}, {bottom.x + out - cut, bottom.y + out},
                {top.x - out + cut, bottom.y + out}, {top.x - out, (top.y + bottom.y) * 0.5f}}};
            dl.AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), tone(border, 0.45f));
            dl.AddPolyline(points.data(), static_cast<int>(points.size()), border, ImDrawFlags_Closed, l.unit * 1.4f);
            if (primary) {
                const float cy = (top.y + bottom.y) * 0.5f;
                for (float direction : {-1.0f, 1.0f}) {
                    const float cx = direction < 0 ? top.x - 10.0f * l.unit : bottom.x + 10.0f * l.unit;
                    dl.AddQuadFilled(ImVec2(cx - 5 * l.unit, cy), ImVec2(cx, cy - 5 * l.unit),
                                     ImVec2(cx + 5 * l.unit, cy), ImVec2(cx, cy + 5 * l.unit), border);
                    dl.AddCircleFilled(ImVec2(cx, cy), l.unit * 1.6f, background, 8);
                }
            }
        }
        dl.AddRectFilled(top, bottom, a.frame_style == "minimal" ? rgb(a.background_color, alpha * 0.45f) : background, rounding);
        const ImVec2 inner(top.x + edge, top.y + edge), inner_end(bottom.x - edge, bottom.y - edge);
        const float right = inner.x + std::max(0.0f, inner_end.x - inner.x) * std::clamp(value->value / 100.0f, 0.0f, 1.0f);
        if (a.texture == "segments") {
            const int count = std::max(1, std::min(10, static_cast<int>((inner_end.x - inner.x) / std::max(2.0f, l.unit * 3.0f))));
            const float segment = (inner_end.x - inner.x) / static_cast<float>(count);
            const float gap = std::min(l.unit * 1.5f, segment * 0.25f);
            for (int i = 0; i < count; ++i) {
                const float x0 = inner.x + static_cast<float>(i) * segment;
                const float x1 = std::min(right, x0 + segment - (i + 1 < count ? gap : 0.0f));
                shaded_fill(ImVec2(x0, inner.y), ImVec2(x1, inner_end.y), rgb(value->color, alpha), 0.0f);
            }
        } else {
            shaded_fill(inner, ImVec2(right, inner_end.y), rgb(value->color, alpha), std::max(0.0f, rounding - edge));
        }
        if (a.frame_style == "minimal") {
            dl.AddLine(ImVec2(top.x, bottom.y), bottom, border, std::max(0.5f, edge));
        } else if (edge > 0.0f) {
            dl.AddRect(ImVec2(top.x + edge * 0.5f, top.y + edge * 0.5f),
                       ImVec2(bottom.x - edge * 0.5f, bottom.y - edge * 0.5f), border, rounding, 0, edge);
        }
        if (primary && target) {
            if (a.target_style == "arrow") {
                dl.AddRect(ImVec2(top.x - l.unit, top.y - l.unit), ImVec2(bottom.x + l.unit, bottom.y + l.unit), border, rounding, 0, l.unit);
            } else if (a.target_style == "chevrons") {
                const float cy = (top.y + bottom.y) * 0.5f;
                for (float direction : {-1.0f, 1.0f}) for (float distance : {8.0f, 15.0f}) {
                    const float cx = direction < 0 ? top.x - distance * l.unit : bottom.x + distance * l.unit;
                    const ImVec2 tip(cx - direction * 3.0f * l.unit, cy);
                    dl.AddLine(ImVec2(cx + direction * 2.0f * l.unit, cy - 5.0f * l.unit), tip, border, l.unit * 2.0f);
                    dl.AddLine(tip, ImVec2(cx + direction * 2.0f * l.unit, cy + 5.0f * l.unit), border, l.unit * 2.0f);
                }
            } else if (a.target_style == "brackets") {
                for (float side : {0.0f, 1.0f}) for (float row : {0.0f, 1.0f}) {
                    const float cx = side == 0 ? top.x - 3.0f * l.unit : bottom.x + 3.0f * l.unit;
                    const float cy = row == 0 ? top.y - 3.0f * l.unit : bottom.y + 3.0f * l.unit;
                    dl.AddLine(ImVec2(cx, cy + (row == 0 ? 5 : -5) * l.unit), ImVec2(cx, cy), border, 1.5f * l.unit);
                    dl.AddLine(ImVec2(cx, cy), ImVec2(cx + (side == 0 ? 7 : -7) * l.unit, cy), border, 1.5f * l.unit);
                }
            } else if (a.target_style == "glow") {
                for (int ring = 1; ring <= 6; ++ring) {
                    const float r = static_cast<float>(ring) * l.unit;
                    dl.AddRect(ImVec2(top.x - r, top.y - r), ImVec2(bottom.x + r, bottom.y + r),
                               rgb(a.border_color, alpha * (0.32f - static_cast<float>(ring) * 0.035f)), rounding + r, 0, l.unit);
                }
            }
        }
        if (!value->text.empty()) {
            const bool inside_health = inside && health;
            const float reserved = std::min(width * 0.5f, text_width(font, l.font * 0.72f, value->text) + l.gap * 2.0f);
            const float label_width = inside_health ? reserved - l.gap : width - l.gap * 2.0f;
            const float label_left = inside_health ? x + width - reserved : x + l.gap;
            text(y + (height - l.font * 0.72f) * 0.5f, l.font * 0.72f, value->text, label_width,
                 label_left, inside_health ? "right" : align);
        }
    };
    bar(a.health, l.health_y, l.health_h, l.width, a.health_text_align, true, true);
    bar(a.mana, l.mana_y, l.mana_h, l.width, a.health_text_align, !a.health, false);
    bar(a.cast, l.cast_y, l.cast_h, l.cast_width, a.cast_text_align, !a.health && !a.mana, false);
    if (a.show_name && inside) {
        const float reserved = a.health->text.empty() ? 0.0f : std::min(l.width * 0.5f,
                                                          text_width(font, l.font * 0.72f, a.health->text) + l.gap * 2.0f);
        text(l.health_y + (l.health_h - l.font) * 0.5f, l.font, a.name ? *a.name : p.name,
             std::max(0.0f, l.width - reserved - l.gap * 2.0f), left + l.gap, a.text_align);
    }
}
} // namespace

glm::vec2 nameplate_size(const NameplateText& p, const NameplateDrawStyle& style) {
    if (p.presentation) {
        if (p.presentation->hide) return {0.0f, 0.0f};
        const auto layout = presentation_layout(p, style);
        return {layout.total_width, layout.height + layout.margin_y * 2.0f};
    }
    ImFont* f = plate_font(style);
    const float size = name_size(p, style), tag = size * style.tag_scale;
    const float bold = style.faux_bold ? 1.0f : 0.0f;
    float w = f->CalcTextSizeA(size, FLT_MAX, 0.0f, p.name.c_str()).x + bold;
    for (const auto& line : p.lines) w = std::max(w, f->CalcTextSizeA(tag, FLT_MAX, 0.0f, line.c_str()).x + bold);
    return {w, size + tag * static_cast<float>(p.lines.size())};
}

void paint_nameplate(ImDrawList& draw, const NameplateText& p, const NameplateDrawStyle& style) {
    if (p.presentation) { paint_presentation(draw, p, style); return; }
    ImDrawList* dl = &draw;
    const float px = p.pos.x, py = p.pos.y;
    // Every colour times the plate's alpha.
    auto fade = [&](ImU32 c) {
        const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(p.alpha, 0.0f, 1.0f);
        return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
    };
    ImFont* f = plate_font(style);
    const float fs = name_size(p, style), tag = fs * style.tag_scale;
    const ImU32 color = fade(p.color);
    // The edge: a drop shadow one pixel down and right (the look before the options), or a dark outline around each
    // letter, thicker for a larger name.
    const ImU32 edge = fade(style.edge == game::PlateEdge::Outline ? IM_COL32(0, 0, 0, 220) : IM_COL32(0, 0, 0, 200));
    const float ring = std::max(1.0f, std::round(fs / 16.0f));
    auto text = [&](ImVec2 at, float size, ImU32 c, const char* s) {
        dl->AddText(f, size, at, c, s);
        if (style.faux_bold) dl->AddText(f, size, ImVec2(at.x + 1.0f, at.y), c, s);
    };
    auto line = [&](float y, float size, const std::string& s) {
        const float w = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str()).x + (style.faux_bold ? 1.0f : 0.0f);
        const ImVec2 at(px - w * 0.5f, y);
        if (style.edge == game::PlateEdge::Shadow) {
            text(ImVec2(at.x + 1, at.y + 1), size, edge, s.c_str());
        } else if (style.edge == game::PlateEdge::Outline) {
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
        dl->AddRectFilled(a, b, fade(IM_COL32(0, 0, 0, 150)), fs * 0.35f);
        dl->AddRect(a, b, color, fs * 0.35f, 0, std::max(1.5f, fs * 0.1f));
        const float bob = std::abs(std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f)) * fs * 0.3f;
        const float tip = a.y - fs * 0.25f - bob, half = fs * 0.4f;
        ImVec2 t0(px - half, tip - half * 1.2f), t1(px + half, tip - half * 1.2f), t2(px, tip);
        dl->AddTriangleFilled(ImVec2(t0.x, t0.y + 1.5f), ImVec2(t1.x, t1.y + 1.5f), ImVec2(t2.x, t2.y + 1.5f), shadow);
        dl->AddTriangleFilled(t0, t1, t2, color);
    }
    line(py, fs, p.name);
    float y = py + fs;
    for (const auto& l : p.lines) {
        line(y, tag, l);
        y += tag;
    }
}

glm::vec2 NameplateLayer::size_of(const NameplateText& plate) const { return nameplate_size(plate, style_); }
void NameplateLayer::paint(const NameplateText& plate) const { paint_nameplate(*dl_, plate, style_); }

glm::vec4 nameplate_bounds(const NameplateText& p, const NameplateDrawStyle& style) {
    if (p.presentation) {
        if (p.presentation->hide) return {p.pos.x, p.pos.y, p.pos.x, p.pos.y};
        const auto layout = presentation_layout(p, style);
        const float left = layout.body_width * 0.5f + layout.margin_x +
                           (p.presentation->badge_position == "left" ? layout.side_extra : 0.0f);
        const float right = layout.body_width * 0.5f + layout.margin_x +
                            (p.presentation->badge_position == "right" ? layout.side_extra : 0.0f);
        return {p.pos.x - left, p.pos.y, p.pos.x + right, p.pos.y + layout.height + layout.margin_y * 2.0f};
    }
    const auto size = nameplate_size(p, style);
    return {p.pos.x - size.x * 0.5f, p.pos.y, p.pos.x + size.x * 0.5f, p.pos.y + size.y};
}

std::optional<std::uint32_t> pick_nameplate(const std::vector<NameplateHit>& hits, glm::vec2 at,
                                           const std::function<bool(const NameplateHit&)>& eligible) {
    if (!std::isfinite(at.x) || !std::isfinite(at.y)) return std::nullopt;
    for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit) {
        const auto& box = hit->bounds;
        if (at.x >= box.x && at.x <= box.z && at.y >= box.y && at.y <= box.w && eligible(*hit)) return hit->id;
    }
    return std::nullopt;
}

bool nameplate_mouse_eligible(const game::MaskSpawn& spawn, std::uint8_t options, bool self, int camera) {
    const auto mask = game::spawn_pick_mask(spawn, options);
    if ((mask & game::kClickSkip) != 0 || (mask & game::kClickRequired) == 0) return false;
    game::PickCallInput in;
    in.camera_type = camera;
    in.click_through = options;
    return !self || !game::pick_call(in).exclude_view;
}

std::optional<float> nameplate_scene_depth(const game::PickSegment& ray, const game::PickScene& scene,
                                          const glm::vec3& forward) {
    float nearest = 1.0f;
    const auto triangle = [&](const game::PickTriangle& t) {
        // Picking's exact posed triangles and the zone/door meshes, tested conservatively from either side.
        // Click-through and passable flags do not make geometry disappear from the scene depth buffer.
        const auto hit = game::segment_triangle_distance(ray, t.v0, t.v1, t.v2);
        if (hit.distance_squared <= game::kPickHitDistanceSquared && hit.t >= 0.0f) nearest = std::min(nearest, hit.t);
    };
    if (scene.world) scene.world(ray, triangle);
    const glm::dvec3 precise_dir(ray.dir);
    const double length_squared = glm::dot(precise_dir, precise_dir);
    for (std::size_t i = 0; i < scene.actors.size(); ++i) {
        const auto& actor = scene.actors[i];
        if (actor.disabled || actor.invisible || static_cast<int>(i) == scene.exclude_a || static_cast<int>(i) == scene.exclude_b) continue;
        if (actor.triangles) {
            if (game::point_segment_distance_squared(ray, actor.bound_center) > actor.bound_radius * actor.bound_radius + 0.01f) continue;
            actor.triangles(triangle);
        } else if (actor.volume == 2 && actor.sphere_radius > 0.0f && length_squared > 0.0f) {
            const glm::dvec3 offset = glm::dvec3(ray.start) - glm::dvec3(actor.position);
            const double b = glm::dot(offset, precise_dir);
            const double c = glm::dot(offset, offset) - static_cast<double>(actor.sphere_radius) * actor.sphere_radius;
            const double discriminant = b * b - length_squared * c;
            if (discriminant < 0.0f) continue;
            const double far = (-b + std::sqrt(discriminant)) / length_squared;
            if (far < 0.0f) continue;
            const double near = std::max(0.0, (-b - std::sqrt(discriminant)) / length_squared);
            nearest = std::min(nearest, static_cast<float>(near));
        }
    }
    if (nearest >= 1.0f) return std::nullopt;
    return glm::dot(ray.dir * nearest, forward);
}

std::optional<game::PickSegment> nameplate_click_ray(const glm::mat4& inverse_scene, const glm::vec4& view,
                                                    const glm::vec3& camera, glm::vec2 at) {
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || view.z <= 0.0f || view.w <= 0.0f ||
        at.x < view.x || at.x >= view.x + view.z || at.y < view.y || at.y >= view.y + view.w) return std::nullopt;
    const glm::vec2 ndc((at.x - view.x) / view.z * 2.0f - 1.0f, 1.0f - (at.y - view.y) / view.w * 2.0f);
    const glm::vec4 far_clip = inverse_scene * glm::vec4(ndc, 0.0f, 1.0f);
    if (!std::isfinite(far_clip.w) || std::abs(far_clip.w) < 1.0e-9f) return std::nullopt;
    game::PickSegment ray{camera, glm::vec3(far_clip) / far_clip.w - camera};
    if (!std::isfinite(ray.dir.x) || !std::isfinite(ray.dir.y) || !std::isfinite(ray.dir.z) || glm::dot(ray.dir, ray.dir) <= 0.0f)
        return std::nullopt;
    return ray;
}

bool nameplate_depth_visible(float plate_depth, const std::optional<float>& scene_depth) {
    if (!std::isfinite(plate_depth) || plate_depth <= 0.0f) return false;
    if (!scene_depth) return true;
    return std::isfinite(*scene_depth) && *scene_depth >= 0.0f && plate_depth <= *scene_depth + 3.0f;
}

void stack_nameplate_boxes(std::vector<game::PlateBox>& boxes, const std::vector<NameplateStack>& settings) {
    if (boxes.size() != settings.size()) return;
    if (std::none_of(settings.begin(), settings.end(), [](const NameplateStack& s) { return s.enabled && s.max_offset > 0.0f; })) return;
    auto order = game::plate_draw_order(boxes);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (boxes[a].front != boxes[b].front) return boxes[a].front;
        if (boxes[a].depth != boxes[b].depth) return boxes[a].depth < boxes[b].depth;
        return settings[a].id < settings[b].id;
    });
    std::vector<std::size_t> pinned;
    pinned.reserve(order.size());
    std::size_t stacked = 0;
    for (std::size_t i : order) {
        auto& box = boxes[i];
        const auto& setting = settings[i];
        if (setting.enabled && !box.front && setting.max_offset > 0.0f && stacked < 256) {
            ++stacked;
            const float top = box.y0;
            std::size_t checks = 0;
            bool moved = true;
            // Crowded raids cannot turn stacking into unbounded work. Any overlaps left at the budget or height
            // limit retain the ordinary overlap fade. Each frame starts from the projected head again.
            while (moved && checks < 4096) {
                moved = false;
                for (std::size_t j : pinned) {
                    if (++checks > 4096) break;
                    const auto& other = boxes[j];
                    if (box.x1 <= other.x0 || box.x0 >= other.x1 || box.y1 + setting.gap <= other.y0 ||
                        box.y0 >= other.y1 + setting.gap) continue;
                    const float up = std::min(box.y1 + setting.gap - other.y0, setting.max_offset - (top - box.y0));
                    if (up <= 0.0f) continue;
                    box.y0 -= up;
                    box.y1 -= up;
                    moved = true;
                }
            }
        }
        pinned.push_back(i);
    }
}

} // namespace hailstone::app
