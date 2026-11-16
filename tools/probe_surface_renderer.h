#pragma once

// Test-tool-only opaque geometry: real triangle rasterization into the scene depth attachment. Production addon
// geometry keeps its own no-depth-write renderer; this is the synthetic scenery behind the draping capture.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>

#include "game/world_geometry.h"
#include "render/vk_context.h"
#include "render/vk_util.h"

class ProbeSurfaceRenderer {
public:
    ProbeSurfaceRenderer(hailstone::render::VkContext& vk, const std::filesystem::path& shaders,
                         std::span<const hailstone::game::WorldGeometryVertex> vertices) : vk_(vk) {
        using Vertex=hailstone::game::WorldGeometryVertex;
        try {
            count_=static_cast<std::uint32_t>(vertices.size());
            VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer.size=vertices.size_bytes();
            buffer.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo allocation{};
            allocation.usage=VMA_MEMORY_USAGE_AUTO;
            allocation.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapped{};
            hailstone::render::vk_check(vmaCreateBuffer(vk_.allocator(),&buffer,&allocation,&buffer_,&allocation_,&mapped),
                                       "create synthetic surface buffer");
            if(!mapped.pMappedData)throw std::runtime_error("surface fixture upload is not mapped");
            std::memcpy(mapped.pMappedData,vertices.data(),vertices.size_bytes());
            hailstone::render::vk_check(vmaFlushAllocation(vk_.allocator(),allocation_,0,vertices.size_bytes()),"flush surface fixture");
            const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(glm::mat4)};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.pushConstantRangeCount=1;
            layout.pPushConstantRanges=&push;
            hailstone::render::vk_check(vkCreatePipelineLayout(vk_.device(),&layout,nullptr,&layout_),"create surface layout");
            vertex_=hailstone::render::create_shader_module(vk_.device(),shaders/"world_geometry.vert.spv");
            fragment_=hailstone::render::create_shader_module(vk_.device(),shaders/"world_geometry.frag.spv");
            const VkPipelineShaderStageCreateInfo stages[]={
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vertex_,"main",nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fragment_,"main",nullptr}};
            const VkVertexInputBindingDescription binding{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX};
            const VkVertexInputAttributeDescription attributes[]={
                {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)},
                {1,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,color)}};
            VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertex.vertexBindingDescriptionCount=1;
            vertex.pVertexBindingDescriptions=&binding;
            vertex.vertexAttributeDescriptionCount=2;
            vertex.pVertexAttributeDescriptions=attributes;
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount=viewport.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode=VK_POLYGON_MODE_FILL;
            raster.cullMode=VK_CULL_MODE_NONE;
            raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples=vk_.scene_samples();
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;
            depth.depthCompareOp=VK_COMPARE_OP_GREATER_OR_EQUAL;
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.colorWriteMask=0xf;
            const auto blends=vk_.scene_blends(attachment,hailstone::render::SceneOutput::Reactive);
            const auto blend=blends.info();
            const VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount=2;
            dynamic.pDynamicStates=states;
            auto rendering=vk_.scene_rendering();
            VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipeline.pNext=&rendering;
            pipeline.stageCount=2;
            pipeline.pStages=stages;
            pipeline.pVertexInputState=&vertex;
            pipeline.pInputAssemblyState=&assembly;
            pipeline.pViewportState=&viewport;
            pipeline.pRasterizationState=&raster;
            pipeline.pMultisampleState=&samples;
            pipeline.pDepthStencilState=&depth;
            pipeline.pColorBlendState=&blend;
            pipeline.pDynamicState=&dynamic;
            pipeline.layout=layout_;
            hailstone::render::vk_check(vkCreateGraphicsPipelines(vk_.device(),vk_.pipeline_cache(),1,&pipeline,nullptr,&pipeline_),
                                       "create synthetic surface pipeline");
        } catch(...) {release();throw;}
    }
    ~ProbeSurfaceRenderer(){vk_.wait_idle();release();}
    ProbeSurfaceRenderer(const ProbeSurfaceRenderer&)=delete;
    void record(const hailstone::render::FrameInfo& frame,const glm::mat4& view) const {
        vkCmdBindPipeline(frame.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
        const VkDeviceSize offset=0;
        vkCmdBindVertexBuffers(frame.cmd,0,1,&buffer_,&offset);
        vkCmdPushConstants(frame.cmd,layout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(view),&view);
        vkCmdDraw(frame.cmd,count_,1,0,0);
    }
private:
    void release(){
        if(pipeline_)vkDestroyPipeline(vk_.device(),pipeline_,nullptr);
        if(layout_)vkDestroyPipelineLayout(vk_.device(),layout_,nullptr);
        if(fragment_)vkDestroyShaderModule(vk_.device(),fragment_,nullptr);
        if(vertex_)vkDestroyShaderModule(vk_.device(),vertex_,nullptr);
        if(buffer_)vmaDestroyBuffer(vk_.allocator(),buffer_,allocation_);
    }
    hailstone::render::VkContext& vk_;
    VkBuffer buffer_=VK_NULL_HANDLE;
    VmaAllocation allocation_{};
    VkPipelineLayout layout_=VK_NULL_HANDLE;
    VkPipeline pipeline_=VK_NULL_HANDLE;
    VkShaderModule vertex_=VK_NULL_HANDLE,fragment_=VK_NULL_HANDLE;
    std::uint32_t count_=0;
};
