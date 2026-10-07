#include <Vulkan_Pipeline.hpp>
#include <Vulkan_Pipeline_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <array>
#include <iterator>
#include <stdexcept>
#include <cassert>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Vulkan_Pipeline::Vulkan_Pipeline(const Vulkan_Device& _device, VkRenderPass _render_pass, uint32_t _subpass_count,
        VkPipelineCache _pipeline_cache, VkPipelineLayout _pipeline_layout, Pipeline_Config _config)
        : pipeline()
    {
        const VkDevice device_handle = _device.Get_logical_device_handle();

        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a pipeline");
        assert(_render_pass != VK_NULL_HANDLE &&
            "Vulkan_Pipeline: the render pass must be created first");
        assert(_pipeline_layout != VK_NULL_HANDLE &&
            "Vulkan_Pipeline: shared pipeline layout must be created first");
        assert(!_config.vertex_shader_path.empty() &&
            "Pipeline_Config: vertex_shader_path must not be empty");
        assert(!_config.fragment_shader_path.empty() &&
            "Pipeline_Config: fragment_shader_path must not be empty");

        // Enforced in every build: a count outside the array would read past
        // color_blend, and a pipeline whose count differs from its
        // subpass's is invalid usage.
        if (_config.color_attachment_count == 0 || _config.color_attachment_count > Pipeline_Config::MAX_COLOR_ATTACHMENTS)
            throw std::invalid_argument("Pipeline_Config: color_attachment_count must be between 1 and MAX_COLOR_ATTACHMENTS");

        if (_config.subpass >= _subpass_count)
            throw std::invalid_argument("Pipeline_Config: subpass is not a subpass of the render pass");

        // ---------- Shader modules ----------
        // Owned by RAII wrappers: if the second module (or anything after
        // it) throws, the first one is destroyed on unwinding instead of
        // leaking.
        const Unique_Shader_Module vertex_shader_module = Vulkan_Pipeline_Utils::Create_shader_module(device_handle, _config.vertex_shader_path);
        const Unique_Shader_Module fragment_shader_module = Vulkan_Pipeline_Utils::Create_shader_module(device_handle, _config.fragment_shader_path);

        VkPipelineShaderStageCreateInfo vertex_stage_info{};
        vertex_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertex_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertex_stage_info.module = vertex_shader_module.Get();
        vertex_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo fragment_stage_info{};
        fragment_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragment_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragment_stage_info.module = fragment_shader_module.Get();
        fragment_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo shader_stages[] = {
            vertex_stage_info,
            fragment_stage_info
        };

        // ---------- Vertex input ----------
        // Described by the config: whoever owns the vertex types maps them
        // to bindings and attributes. An empty description declares no
        // binding and no attribute: the vertex shader builds its positions
        // from gl_VertexIndex, and the vertex buffer bound for the meshes
        // is simply not read. _config is a copy that outlives the call to
        // vkCreateGraphicsPipelines, so the pointers stay valid.
        const Vertex_Input_State& vertex_input = _config.vertex_input;

        VkPipelineVertexInputStateCreateInfo vertex_input_info{};
        vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_info.vertexBindingDescriptionCount = static_cast<uint32_t>(vertex_input.bindings.size());
        vertex_input_info.pVertexBindingDescriptions = vertex_input.bindings.empty() ? nullptr : vertex_input.bindings.data();
        vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_input.attributes.size());
        vertex_input_info.pVertexAttributeDescriptions = vertex_input.attributes.empty() ? nullptr : vertex_input.attributes.data();

        // ---------- Input assembly ----------
        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        input_assembly.primitiveRestartEnable = VK_FALSE;

        // ---------- Dynamic state ----------
        // Viewport/scissor were already dynamic. Cull, front face and the
        // depth test/write/compare trio join them: all core in Vulkan 1.3,
        // so they no longer force a separate pipeline per combination.
        VkDynamicState dynamic_states[] = 
        {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP
        };

        VkPipelineDynamicStateCreateInfo dynamic_state_info{};
        dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(std::size(dynamic_states));
            
        dynamic_state_info.pDynamicStates = dynamic_states;

        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        // ---------- Rasterizer — driven by Pipeline_Config ----------
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = _config.polygon_mode;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        // ---------- Multisampling ----------
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // ---------- Depth/stencil — driven by Pipeline_Config ----------
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_FALSE;
        depth_stencil.depthWriteEnable = VK_FALSE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_NEVER;
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.stencilTestEnable = VK_FALSE;

        // ---------- Color blending — driven by Pipeline_Config ----------
        // One state per color attachment of the subpass. The write mask
        // keeps every component; the components a format lacks (GBA of a
        // one-channel target) are ignored.
        std::array<VkPipelineColorBlendAttachmentState, Pipeline_Config::MAX_COLOR_ATTACHMENTS> color_blend_attachments{};

        for (uint32_t i = 0; i < _config.color_attachment_count; ++i)
        {
            const Color_Blend_State& state = _config.color_blend[i];
            VkPipelineColorBlendAttachmentState& attachment = color_blend_attachments[i];

            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            attachment.blendEnable = state.blend_enable ? VK_TRUE : VK_FALSE;
            attachment.srcColorBlendFactor = state.src_color_blend_factor;
            attachment.dstColorBlendFactor = state.dst_color_blend_factor;
            attachment.colorBlendOp = state.color_blend_op;
            attachment.srcAlphaBlendFactor = state.src_alpha_blend_factor;
            attachment.dstAlphaBlendFactor = state.dst_alpha_blend_factor;
            attachment.alphaBlendOp = state.alpha_blend_op;
        }

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.logicOpEnable = VK_FALSE;
        color_blending.attachmentCount = _config.color_attachment_count;
        color_blending.pAttachments = color_blend_attachments.data();

        // ---------- Creation feedback (core in Vulkan 1.3) ----------
        // The only way to know whether the cache is actually being hit
        // instead of assuming it. Must be chained BEFORE creation.
        // Not const: the driver writes the feedback into it during creation.
        Vulkan_Pipeline_Utils::Creation_Feedback feedback(2);

        // ---------- Pipeline creation ----------
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = feedback.Get_create_info();

        pipeline_info.stageCount = 2;
        pipeline_info.pStages = shader_stages;
        pipeline_info.pVertexInputState = &vertex_input_info;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterizer;
        pipeline_info.pMultisampleState = &multisampling;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &color_blending;
        pipeline_info.pDynamicState = &dynamic_state_info;
        pipeline_info.layout = _pipeline_layout;
        pipeline_info.renderPass = _render_pass;
        pipeline_info.subpass = _config.subpass;

        // The shader modules are destroyed by their wrappers when this
        // constructor returns or throws; the pipeline keeps no reference
        // to them once created. The output of a failed creation is
        // undefined: the owner is built only from a successful one.
        VkPipeline created = VK_NULL_HANDLE;
        VK_CHECK(vkCreateGraphicsPipelines(device_handle, _pipeline_cache, 1, &pipeline_info, nullptr, &created),
            "Failed to create graphics pipeline");

        pipeline = Unique_Pipeline(device_handle, created);

        feedback.Log("Graphics pipeline");
    }

    // ---------- Getters ----------
    VkPipeline Vulkan_Pipeline::Get_handle() const
    {
        assert(pipeline &&
            "Get_handle() called on a moved-from or destroyed Vulkan_Pipeline");
        return pipeline.Get();
    }

} // namespace Renderer
