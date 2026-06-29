#include <Vulkan_Pipeline.hpp>
#include <Vulkan_Vertex_Layout.hpp>
#include <Vulkan_Utils.hpp>
#include <Filesystem.hpp>

#include <glm/glm.hpp>

#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer
{

    // ---------- Constructor ----------
    Vulkan_Pipeline::Vulkan_Pipeline(
        const Vulkan_Device& _device,
        const Vulkan_Render_Pass& _render_pass,
        Pipeline_Config           _config)

        : device_handle(_device.Get_logical_device_handle()),
        descriptor_set_layout(VK_NULL_HANDLE),
        pipeline_layout(VK_NULL_HANDLE),
        pipeline(VK_NULL_HANDLE)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a pipeline");
        assert(!_config.vertex_shader_path.empty() &&
            "Pipeline_Config: vertex_shader_path must not be empty");
        assert(!_config.fragment_shader_path.empty() &&
            "Pipeline_Config: fragment_shader_path must not be empty");

        Create_descriptor_set_layout();
        Create_pipeline_layout();

        // ---------- Shader modules ----------
        VkShaderModule vertex_shader_module = Create_shader_module(_config.vertex_shader_path);
        VkShaderModule fragment_shader_module = Create_shader_module(_config.fragment_shader_path);

        VkPipelineShaderStageCreateInfo vertex_stage_info{};
        vertex_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertex_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertex_stage_info.module = vertex_shader_module;
        vertex_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo fragment_stage_info{};
        fragment_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragment_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragment_stage_info.module = fragment_shader_module;
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

        // ---------- Dynamic state (viewport + scissor) ----------
        VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };

        VkPipelineDynamicStateCreateInfo dynamic_state_info{};
        dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_info.dynamicStateCount = 2;
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
        rasterizer.cullMode = _config.cull_mode;
        rasterizer.frontFace = _config.front_face;
        rasterizer.depthBiasEnable = VK_FALSE;

        // ---------- Multisampling ----------
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // ---------- Depth/stencil — driven by Pipeline_Config ----------
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = _config.depth_test_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = _config.depth_write_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = _config.depth_compare_op;
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.stencilTestEnable = VK_FALSE;

        // ---------- Color blending — driven by Pipeline_Config ----------
        VkPipelineColorBlendAttachmentState color_blend_attachment{};
        color_blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
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

        // ---------- Pipeline creation ----------
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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
        pipeline_info.layout = pipeline_layout;
        pipeline_info.renderPass = _render_pass.Get_handle();
        pipeline_info.subpass = 0;

        VkResult result = vkCreateGraphicsPipelines(
            device_handle,
            VK_NULL_HANDLE,
            1,
            &pipeline_info,
            nullptr,
            &pipeline
        );

        vkDestroyShaderModule(device_handle, fragment_shader_module, nullptr);
        vkDestroyShaderModule(device_handle, vertex_shader_module, nullptr);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create graphics pipeline: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        std::cout << "[Vulkan_Pipeline] Graphics pipeline created successfully.\n";
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
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_handle, pipeline_layout, nullptr);
            pipeline_layout = VK_NULL_HANDLE;
        }
        if (descriptor_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_handle, descriptor_set_layout, nullptr);
            descriptor_set_layout = VK_NULL_HANDLE;
        }
    }

    // ---------- Move constructor ----------
    Vulkan_Pipeline::Vulkan_Pipeline(Vulkan_Pipeline&& _other) noexcept
        : device_handle(_other.device_handle),
        descriptor_set_layout(_other.descriptor_set_layout),
        pipeline_layout(_other.pipeline_layout),
        pipeline(_other.pipeline)
    {
        _other.descriptor_set_layout = VK_NULL_HANDLE;
        _other.pipeline_layout = VK_NULL_HANDLE;
        _other.pipeline = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Pipeline& Vulkan_Pipeline::operator=(Vulkan_Pipeline&& _other) noexcept
    {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            descriptor_set_layout = _other.descriptor_set_layout;
            pipeline_layout = _other.pipeline_layout;
            pipeline = _other.pipeline;

            _other.descriptor_set_layout = VK_NULL_HANDLE;
            _other.pipeline_layout = VK_NULL_HANDLE;
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

    VkPipelineLayout Vulkan_Pipeline::Get_layout_handle() const
    {
        assert(pipeline_layout != VK_NULL_HANDLE &&
            "Get_layout_handle() called on a moved-from or destroyed Vulkan_Pipeline");
        return pipeline_layout;
    }

    VkDescriptorSetLayout Vulkan_Pipeline::Get_descriptor_set_layout() const
    {
        assert(descriptor_set_layout != VK_NULL_HANDLE &&
            "Get_descriptor_set_layout() called on a moved-from or destroyed Vulkan_Pipeline");
        return descriptor_set_layout;
    }

    // ---------- Create_descriptor_set_layout ----------
    void Vulkan_Pipeline::Create_descriptor_set_layout()
    {
        VkDescriptorSetLayoutBinding mvp_layout_binding{};
        mvp_layout_binding.binding = 0;
        mvp_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mvp_layout_binding.descriptorCount = 1;
        mvp_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        mvp_layout_binding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &mvp_layout_binding;

        VkResult result = vkCreateDescriptorSetLayout(
            device_handle, &layout_info, nullptr, &descriptor_set_layout);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create descriptor set layout: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }
    }

    // ---------- Create_pipeline_layout ----------
    void Vulkan_Pipeline::Create_pipeline_layout()
    {
        // Push constant range for the per-draw model matrix.
        // The vertex shader declares:
        //   layout(push_constant) uniform Push_Constants { mat4 model; } push;
        // 64 bytes (one mat4) is well within the guaranteed 128-byte minimum
        // push constant size, so this is portable across all Vulkan devices.
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(glm::mat4);   // 64 bytes

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &descriptor_set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;

        VkResult result = vkCreatePipelineLayout(
            device_handle, &layout_info, nullptr, &pipeline_layout);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create pipeline layout: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }
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

        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = shader_code.size();
        create_info.pCode = reinterpret_cast<const uint32_t*>(shader_code.data());

        VkShaderModule shader_module;
        VkResult result = vkCreateShaderModule(device_handle, &create_info, nullptr, &shader_module);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create shader module from '" + _spv_file_path + "': " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        return shader_module;
    }

} // namespace Renderer