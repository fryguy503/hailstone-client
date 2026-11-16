#include "render/world_geometry_renderer.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "render/vk_util.h"

namespace hailstone::render {

namespace {

using Vertex = game::WorldGeometryVertex;
static_assert(std::is_standard_layout_v<Vertex>);
static_assert(std::is_trivially_copyable_v<Vertex>);
static_assert(sizeof(Vertex) == 7 * sizeof(float));
static_assert(sizeof(glm::mat4) == 16 * sizeof(float));

struct ShaderModules {
    VkDevice device;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    ~ShaderModules() {
        if (fragment) vkDestroyShaderModule(device, fragment, nullptr);
        if (vertex) vkDestroyShaderModule(device, vertex, nullptr);
    }
};

} // namespace

bool world_geometry_view_valid(const glm::mat4& matrix) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max()) / 2.0;
    for (int row = 0; row < 4; ++row) {
        double magnitude = 0.0;
        for (int column = 0; column < 4; ++column) {
            const float value = matrix[column][row];
            if (!std::isfinite(value)) return false;
            const double coordinate = column < 3 ? game::kWorldGeometryMaxCoordinate : 1.0;
            magnitude += std::abs(static_cast<double>(value)) * coordinate;
        }
        if (magnitude > maximum) return false; // margin for float multiplication and addition rounding
    }
    return true;
}

WorldGeometryRenderer::WorldGeometryRenderer(VkContext& vk, const std::filesystem::path& shader_dir) : vk_(vk) {
    try {
        for (std::size_t index = 0; index < vertices_.size(); ++index) {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = sizeof(Vertex) * game::kWorldGeometryMaxVertices;
            info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_AUTO;
            allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo output{};
            vk_check(vmaCreateBuffer(vk_.allocator(), &info, &allocation, &vertices_[index].buffer,
                                     &vertices_[index].allocation, &output),
                     "create world geometry buffer");
            vertices_[index].size = info.size;
            mapped_[index] = output.pMappedData;
            if (!mapped_[index]) throw std::runtime_error("world geometry buffer is not mapped");
        }
        create_pipelines(shader_dir);
    } catch (...) {
        // No commands can refer to a constructor that did not return.
        release_resources();
        throw;
    }
}

WorldGeometryRenderer::~WorldGeometryRenderer() {
    vk_.wait_idle();
    release_resources();
}

void WorldGeometryRenderer::release_resources() {
    for (auto& pipeline : pipelines_) {
        if (pipeline) vkDestroyPipeline(vk_.device(), pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(vk_.device(), layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    for (auto& buffer : vertices_) buffer.destroy(vk_.allocator());
}

WorldGeometryBatchResult WorldGeometryRenderer::record(const FrameInfo& frame, const glm::mat4& view_proj,
                                                      const game::WorldGeometryBatch& batch) {
    WorldGeometryBatchResult result;
    result.id = batch.id;
    result.frame = vk_.frames_begun();
    if (frame.cmd == VK_NULL_HANDLE || frame.frame_slot >= frames_.size() || !frame.extent.width ||
        !frame.extent.height || result.frame == 0 || frame.frame_slot != vk_.next_frame_slot()) {
        result.status = WorldGeometryRecordStatus::InvalidFrame;
        return result;
    }
    if (!world_geometry_view_valid(view_proj)) {
        result.status = WorldGeometryRecordStatus::InvalidView;
        return result;
    }
    auto& state = frames_[frame.frame_slot];
    if (state.generation != result.frame) {
        // begin_frame_commands has waited for this slot's fence before making
        // its new generation visible. The other slot is never overwritten.
        state.generation = result.frame;
        state.budget = {};
    }
    result.validation = state.budget.admit(batch);
    if (result.validation != game::WorldGeometryValidation::Valid) return result;
    const auto count = static_cast<std::uint32_t>(batch.vertices.size());
    const auto first = state.budget.reserved_vertices() - count;
    const std::size_t offset = static_cast<std::size_t>(first) * sizeof(Vertex);
    const std::size_t bytes = static_cast<std::size_t>(count) * sizeof(Vertex);
    std::memcpy(static_cast<std::byte*>(mapped_[frame.frame_slot]) + offset, batch.vertices.data(), bytes);
    if (vmaFlushAllocation(vk_.allocator(), vertices_[frame.frame_slot].allocation,
                           static_cast<VkDeviceSize>(offset), static_cast<VkDeviceSize>(bytes)) != VK_SUCCESS) {
        result.status = WorldGeometryRecordStatus::UploadFailed;
        return result;
    }
    const std::size_t pipeline = batch.primitive == game::WorldGeometryPrimitive::Triangles ? 0 : 1;
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[pipeline]);
    const VkDeviceSize buffer_offset = 0;
    vkCmdBindVertexBuffers(frame.cmd, 0, 1, &vertices_[frame.frame_slot].buffer, &buffer_offset);
    vkCmdPushConstants(frame.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(view_proj), &view_proj);
    vkCmdDraw(frame.cmd, count, 1, first, 0);
    result.recorded_vertices = count;
    result.status = WorldGeometryRecordStatus::Recorded;
    return result;
}

void WorldGeometryRenderer::create_pipelines(const std::filesystem::path& shader_dir) {
    const VkDevice device = vk_.device();
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    vk_check(vkCreatePipelineLayout(device, &layout, nullptr, &layout_),
             "vkCreatePipelineLayout (world geometry)");
    ShaderModules modules{device};
    modules.vertex = create_shader_module(device, shader_dir / "world_geometry.vert.spv");
    modules.fragment = create_shader_module(device, shader_dir / "world_geometry.frag.spv");
    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         modules.vertex, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         modules.fragment, "main", nullptr},
    };
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 2;
    vertex.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f; // the portable line width; wider outlines are producer-authored triangles
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = vk_.scene_samples();
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xf;
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    const SceneBlends blends = vk_.scene_blends(attachment, SceneOutput::Reactive);
    const VkPipelineColorBlendStateCreateInfo blend = blends.info();
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    // Both classic and modern draw into these same linear scene attachments;
    // there is no dependence on begin_translucent's copied color/depth images.
    VkPipelineRenderingCreateInfo rendering = vk_.scene_rendering();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    const VkPrimitiveTopology topologies[] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST};
    for (std::size_t index = 0; index < pipelines_.size(); ++index) {
        assembly.topology = topologies[index];
        vk_check(vkCreateGraphicsPipelines(device, vk_.pipeline_cache(), 1, &info, nullptr, &pipelines_[index]),
                 "vkCreateGraphicsPipelines (world geometry)");
    }
}

} // namespace hailstone::render
