#include <Vulkan_Pipeline.hpp>
#include <Vulkan_Vertex_Layout.hpp>
#include <Vulkan_Utils.hpp>
#include <Filesystem.hpp>

#include <glm/glm.hpp>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <iostream>
#include <cassert>
#include <vector>

namespace Renderer_System
{

    namespace
    {
        // Scoped owner of a VkShaderModule. Shader modules are only needed
        // while vkCreateGraphicsPipelines runs; this guarantees they are
        // destroyed on every exit path of the constructor, including an
        // exception thrown by the creation of a later module.
        struct Shader_Module
        {
            VkDevice       device;
            VkShaderModule handle;

            Shader_Module(VkDevice _device, VkShaderModule _handle) : device(_device), handle(_handle) {}
            ~Shader_Module() { if (handle != VK_NULL_HANDLE) vkDestroyShaderModule(device, handle, nullptr); }

            Shader_Module(const Shader_Module&) = delete;
            Shader_Module& operator=(const Shader_Module&) = delete;
        };
    }

    // ---------- Constructor ----------
    Vulkan_Pipeline::Vulkan_Pipeline(const Vulkan_Device& _device, const Vulkan_Render_Pass& _render_pass, VkPipelineCache _pipeline_cache,
        VkPipelineLayout _pipeline_layout, Pipeline_Config  _config) : device_handle(_device.Get_logical_device_handle()),pipeline(VK_NULL_HANDLE)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a pipeline");
        assert(_pipeline_layout != VK_NULL_HANDLE &&
            "Vulkan_Pipeline: shared pipeline layout must be created first");
        assert(!_config.vertex_shader_path.empty() &&
            "Pipeline_Config: vertex_shader_path must not be empty");
        assert(!_config.fragment_shader_path.empty() &&
            "Pipeline_Config: fragment_shader_path must not be empty");

        // ---------- Shader modules ----------
        // Owned by RAII guards: if the second module (or anything after it)
        // throws, the first one is destroyed on unwinding instead of leaking.
        const Shader_Module vertex_shader_module(device_handle, Create_shader_module(_config.vertex_shader_path));
        const Shader_Module fragment_shader_module(device_handle, Create_shader_module(_config.fragment_shader_path));

        VkPipelineShaderStageCreateInfo vertex_stage_info{};
        vertex_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertex_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertex_stage_info.module = vertex_shader_module.handle;
        vertex_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo fragment_stage_info{};
        fragment_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragment_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragment_stage_info.module = fragment_shader_module.handle;
        fragment_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo shader_stages[] = {
            vertex_stage_info,
            fragment_stage_info
        };

        // ---------- Vertex input ----------
        auto binding_description = Vulkan_Vertex_Layout::Get_binding_description<CoreTypes::Vertex_Static_Mesh>();
        auto attribute_descriptions = Vulkan_Vertex_Layout::Get_attribute_descriptions<CoreTypes::Vertex_Static_Mesh>();

        VkPipelineVertexInputStateCreateInfo vertex_input_info{};
        vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_info.vertexBindingDescriptionCount = 1;
        vertex_input_info.pVertexBindingDescriptions = &binding_description;
        vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descriptions.size());
        vertex_input_info.pVertexAttributeDescriptions = attribute_descriptions.data();

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
        VkPipelineColorBlendAttachmentState color_blend_attachment{};
        color_blend_attachment.colorWriteMask =VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            
        color_blend_attachment.blendEnable = _config.blend_enable ? VK_TRUE : VK_FALSE;
        color_blend_attachment.srcColorBlendFactor = _config.src_color_blend_factor;
        color_blend_attachment.dstColorBlendFactor = _config.dst_color_blend_factor;
        color_blend_attachment.colorBlendOp = _config.color_blend_op;
        color_blend_attachment.srcAlphaBlendFactor = _config.src_alpha_blend_factor;
        color_blend_attachment.dstAlphaBlendFactor = _config.dst_alpha_blend_factor;
        color_blend_attachment.alphaBlendOp = _config.alpha_blend_op;

        VkPipelineColorBlendStateCreateInfo color_blending{};
        color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blending.logicOpEnable = VK_FALSE;
        color_blending.attachmentCount = 1;
        color_blending.pAttachments = &color_blend_attachment;

        // ---------- Creation feedback (core in Vulkan 1.3) ----------
        // The only way to know whether the cache is actually being hit
        // instead of assuming it. Must be chained BEFORE creation.
        VkPipelineCreationFeedback pipeline_feedback{};
        VkPipelineCreationFeedback stage_feedbacks[2]{};

        VkPipelineCreationFeedbackCreateInfo feedback_info{};
        feedback_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
        feedback_info.pPipelineCreationFeedback = &pipeline_feedback;
        feedback_info.pipelineStageCreationFeedbackCount = 2;
        feedback_info.pPipelineStageCreationFeedbacks = stage_feedbacks;

        // ---------- Pipeline creation ----------
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &feedback_info;

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
        pipeline_info.renderPass = _render_pass.Get_handle();
        pipeline_info.subpass = 0;

        // The shader modules are destroyed by their guards when this
        // constructor returns or throws; the pipeline keeps no reference
        // to them once created.
        VK_CHECK(vkCreateGraphicsPipelines(device_handle, _pipeline_cache, 1, &pipeline_info, nullptr, &pipeline),
            "Failed to create graphics pipeline");

        // VALID_BIT first: if the driver didn't fill the feedback in, every
        // other bit in it is meaningless.
        if (pipeline_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)
        {
            const bool cache_hit = (pipeline_feedback.flags &
                VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;

            std::cout << "[Vulkan_Pipeline] Graphics pipeline created — "
                << (cache_hit ? "CACHE HIT" : "cache miss (compiled)")
                << ", " << (pipeline_feedback.duration / 1000000.0)
                << " ms.\n";
        }
        else
        {
            std::cout << "[Vulkan_Pipeline] Graphics pipeline created "
                "(driver reported no creation feedback).\n";
        }
    }

    // ---------- Destructor ----------
    Vulkan_Pipeline::~Vulkan_Pipeline()
    {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Pipeline::Destroy()
    {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_handle, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
        
    }

    // ---------- Move constructor ----------
    Vulkan_Pipeline::Vulkan_Pipeline(Vulkan_Pipeline&& _other) noexcept
        : device_handle(_other.device_handle),
        pipeline(_other.pipeline)
    {
        
        _other.pipeline = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Pipeline& Vulkan_Pipeline::operator=(Vulkan_Pipeline&& _other) noexcept
    {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            
            pipeline = _other.pipeline;


            _other.pipeline = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Getters ----------
    VkPipeline Vulkan_Pipeline::Get_handle() const
    {
        assert(pipeline != VK_NULL_HANDLE &&
            "Get_handle() called on a moved-from or destroyed Vulkan_Pipeline");
        return pipeline;
    }

    
    

    // ---------- Create_shader_module ----------
    VkShaderModule Vulkan_Pipeline::Create_shader_module(const std::string& _spv_file_path) const
    {
        std::vector<uint8_t> shader_code = Platform::Filesystem::Read_binary_file(_spv_file_path);

        if (shader_code.empty()) {
            throw std::runtime_error(
                "Failed to read shader file or file is empty: " + _spv_file_path
            );
        }

        if (shader_code.size() % sizeof(uint32_t) != 0) {
            throw std::runtime_error(
                "Shader file is not valid SPIR-V (size is not a multiple of 4): " + _spv_file_path
            );
        }

        // pCode must point at 4-byte aligned words; a std::vector<uint8_t>
        // gives no such guarantee, so the words are copied into a uint32_t
        // vector first.
        std::vector<uint32_t> words(shader_code.size() / sizeof(uint32_t));
        std::memcpy(words.data(), shader_code.data(), shader_code.size());

        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = shader_code.size();
        create_info.pCode = words.data();

        VkShaderModule shader_module = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(device_handle, &create_info, nullptr, &shader_module),
            ("Failed to create shader module from '" + _spv_file_path + "'").c_str());

        return shader_module;
    }

} // namespace Renderer