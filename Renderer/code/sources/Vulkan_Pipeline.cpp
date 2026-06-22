#include <Vulkan_Pipeline.hpp>
#include <Vulkan_Utils.hpp>
#include <Vertex.hpp>
#include <Filesystem.hpp>

#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer {

    // ---------- Constructor ----------
    Vulkan_Pipeline::Vulkan_Pipeline(
        const Vulkan_Device& _device,
        const Vulkan_Render_Pass& _render_pass,
        const std::string& _vertex_shader_path,
        const std::string& _fragment_shader_path)

        : device_handle(_device.Get_logical_device_handle()),
        descriptor_set_layout(VK_NULL_HANDLE),
        pipeline_layout(VK_NULL_HANDLE),
        pipeline(VK_NULL_HANDLE) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a pipeline");

        Create_descriptor_set_layout();
        Create_pipeline_layout();

        // ---------- Shader modules ----------
        VkShaderModule vertex_shader_module = Create_shader_module(_vertex_shader_path);
        VkShaderModule fragment_shader_module = Create_shader_module(_fragment_shader_path);

        VkPipelineShaderStageCreateInfo vertex_stage_info{};
        vertex_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertex_stage_info.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertex_stage_info.module = vertex_shader_module;
        vertex_stage_info.pName = "main"; // entry point function name inside the shader

        VkPipelineShaderStageCreateInfo fragment_stage_info{};
        fragment_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragment_stage_info.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragment_stage_info.module = fragment_shader_module;
        fragment_stage_info.pName = "main";

        VkPipelineShaderStageCreateInfo shader_stages[] = { vertex_stage_info, fragment_stage_info };

        // ---------- Vertex input ----------
        // Describes the layout of Vertex_Static_Mesh, so the GPU knows
        // how to read vertex buffer data and feed it into the vertex shader.
        VkVertexInputBindingDescription binding_description = Vertex_Static_Mesh::Get_binding_description();
        auto attribute_descriptions = Vertex_Static_Mesh::Get_attribute_descriptions();

        VkPipelineVertexInputStateCreateInfo vertex_input_info{};
        vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_info.vertexBindingDescriptionCount = 1;
        vertex_input_info.pVertexBindingDescriptions = &binding_description;
        vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descriptions.size());
        vertex_input_info.pVertexAttributeDescriptions = attribute_descriptions.data();

        // ---------- Input assembly ----------
        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // every 3 vertices form one independent triangle
        input_assembly.primitiveRestartEnable = VK_FALSE;

        // ---------- Dynamic state (viewport + scissor) ----------
        // Marking viewport and scissor as dynamic means they're NOT baked
        // into the pipeline - instead, they're set per-frame via
        // vkCmdSetViewport/vkCmdSetScissor when recording the command
        // buffer. This means a window resize only requires recreating the
        // swapchain/framebuffers, NOT this entire pipeline.
        VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };

        VkPipelineDynamicStateCreateInfo dynamic_state_info{};
        dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_info.dynamicStateCount = 2;
        dynamic_state_info.pDynamicStates = dynamic_states;

        // Viewport/scissor still need a count specified here, even though
        // their actual values are dynamic - the pointers can be null since
        // the real values come from vkCmdSetViewport/vkCmdSetScissor later.
        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        // ---------- Rasterizer ----------
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE; // false = actually run the rasterizer (true would skip rasterization entirely)
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;  // solid fill, not wireframe or point cloud
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;    // don't draw back-facing triangles
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // matches GLM's default winding order
        rasterizer.depthBiasEnable = VK_FALSE;

        // ---------- Multisampling ----------
        // Disabled for now - matches VK_SAMPLE_COUNT_1_BIT used in
        // Vulkan_Render_Pass's attachments.
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // ---------- Depth/stencil testing ----------
        // Enabled, matching the depth attachment Vulkan_Render_Pass was
        // built with - required for correct occlusion between overlapping
        // 3D geometry.
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_TRUE;
        depth_stencil.depthWriteEnable = VK_TRUE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS; // closer fragments (smaller depth) win
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.stencilTestEnable = VK_FALSE; // not using stencil yet

        // ---------- Color blending ----------
        // No blending for now - fully opaque output. Transparency will
        // need a different blend state, applied later for objects
        // specifically marked as transparent.
        VkPipelineColorBlendAttachmentState color_blend_attachment{};
        color_blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        color_blend_attachment.blendEnable = VK_FALSE;

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
        pipeline_info.subpass = 0; // index of the subpass this pipeline is used in (we only have one)

        VkResult result = vkCreateGraphicsPipelines(
            device_handle,
            VK_NULL_HANDLE, // pipeline cache - none for now, could speed up repeated pipeline creation later
            1,
            &pipeline_info,
            nullptr,
            &pipeline
        );

        // Shader modules are only needed during pipeline creation - safe
        // to destroy them immediately afterward regardless of success/failure.
        vkDestroyShaderModule(device_handle, fragment_shader_module, nullptr);
        vkDestroyShaderModule(device_handle, vertex_shader_module, nullptr);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create graphics pipeline: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        std::cout << "[Vulkan_Pipeline] Graphics pipeline created successfully.\n";
    }

    // ---------- Destructor ----------
    Vulkan_Pipeline::~Vulkan_Pipeline() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Pipeline::Destroy() {
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
        pipeline(_other.pipeline) {

        _other.descriptor_set_layout = VK_NULL_HANDLE;
        _other.pipeline_layout = VK_NULL_HANDLE;
        _other.pipeline = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Pipeline& Vulkan_Pipeline::operator=(Vulkan_Pipeline&& _other) noexcept {
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

    // ---------- Get_handle ----------
    VkPipeline Vulkan_Pipeline::Get_handle() const {
        assert(pipeline != VK_NULL_HANDLE && "Get_handle() called on a moved-from or destroyed Vulkan_Pipeline");
        return pipeline;
    }

    // ---------- Get_layout_handle ----------
    VkPipelineLayout Vulkan_Pipeline::Get_layout_handle() const {
        assert(pipeline_layout != VK_NULL_HANDLE && "Get_layout_handle() called on a moved-from or destroyed Vulkan_Pipeline");
        return pipeline_layout;
    }

    // ---------- Get_descriptor_set_layout ----------
    VkDescriptorSetLayout Vulkan_Pipeline::Get_descriptor_set_layout() const {
        assert(descriptor_set_layout != VK_NULL_HANDLE && "Get_descriptor_set_layout() called on a moved-from or destroyed Vulkan_Pipeline");
        return descriptor_set_layout;
    }

    // ---------- Create_descriptor_set_layout ----------
    void Vulkan_Pipeline::Create_descriptor_set_layout() {
        // Describes ONE binding: a uniform buffer at binding = 0,
        // intended to hold Model/View/Projection matrices, visible to
        // the vertex shader (where MVP transformation happens).
        VkDescriptorSetLayoutBinding mvp_layout_binding{};
        mvp_layout_binding.binding = 0;
        mvp_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        mvp_layout_binding.descriptorCount = 1;
        mvp_layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        mvp_layout_binding.pImmutableSamplers = nullptr; // not a sampler, irrelevant here

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &mvp_layout_binding;

        VkResult result = vkCreateDescriptorSetLayout(device_handle, &layout_info, nullptr, &descriptor_set_layout);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create descriptor set layout: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }
    }

    // ---------- Create_pipeline_layout ----------
    void Vulkan_Pipeline::Create_pipeline_layout() {
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &descriptor_set_layout;
        layout_info.pushConstantRangeCount = 0; // not using push constants yet

        VkResult result = vkCreatePipelineLayout(device_handle, &layout_info, nullptr, &pipeline_layout);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create pipeline layout: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }
    }

    // ---------- Create_shader_module ----------
    VkShaderModule Vulkan_Pipeline::Create_shader_module(const std::string& _spv_file_path) const {
        std::vector<uint8_t> shader_code = Platform::Filesystem::Read_binary_file(_spv_file_path);

        if (shader_code.empty()) {
            throw std::runtime_error("Failed to read shader file or file is empty: " + _spv_file_path);
        }

        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = shader_code.size();

        // SPIR-V bytecode is structured as 32-bit words, but we read it
        // as a byte array - reinterpret_cast here is safe because
        // std::vector<uint8_t> guarantees contiguous, properly aligned memory.
        create_info.pCode = reinterpret_cast<const uint32_t*>(shader_code.data());

        VkShaderModule shader_module;
        VkResult result = vkCreateShaderModule(device_handle, &create_info, nullptr, &shader_module);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create shader module from '" + _spv_file_path + "': " +
                Vulkan_utils::Vk_result_to_string(result)
            );
        }

        return shader_module;
    }

}