#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Render_Pass.hpp>

#include <string>

namespace Renderer
{

    // Pipeline_Config: all parameters needed to create a Vulkan_Pipeline.
    // Passed by value to the constructor so the pipeline interface stays
    // stable as new fields are added (e.g. blend mode, cull mode, push
    // constant ranges, extra descriptor set layouts for PBR textures).
    // Fields that don't change between pipelines use sensible defaults.
    struct Pipeline_Config
    {
        // ── Shaders (required) ────────────────────────────────────────
        std::string vertex_shader_path;
        std::string fragment_shader_path;

        // ── Rasterization ─────────────────────────────────────────────
        VkPolygonMode   polygon_mode = VK_POLYGON_MODE_FILL;
        VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
        VkFrontFace     front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        // ── Depth ─────────────────────────────────────────────────────
        bool          depth_test_enable = true;
        bool          depth_write_enable = true;
        VkCompareOp   depth_compare_op = VK_COMPARE_OP_LESS;

        // ── Blending ──────────────────────────────────────────────────
        // false = fully opaque (standard for solid geometry).
        // true  = alpha blending (set src/dst factors below).
        bool                  blend_enable = false;
        VkBlendFactor         src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA;
        VkBlendFactor         dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkBlendOp             color_blend_op = VK_BLEND_OP_ADD;
        VkBlendFactor         src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        VkBlendFactor         dst_alpha_blend_factor = VK_BLEND_FACTOR_ZERO;
        VkBlendOp             alpha_blend_op = VK_BLEND_OP_ADD;
    };

    // Vulkan_Pipeline: owns the VkPipeline (graphics pipeline) along with
    // its VkPipelineLayout and VkDescriptorSetLayout.
    //
    // The pipeline bundles together everything needed to rasterize
    // geometry: which shaders to run, how to interpret vertex data, what
    // primitive type to draw, rasterization settings, depth testing,
    // color blending, and which render pass it's compatible with. Unlike
    // OpenGL, all of this state is fixed into a single immutable object
    // at creation time.
    //
    // Also creates a descriptor set layout for a single uniform buffer
    // (intended for Model/View/Projection matrices) — the actual uniform
    // buffer itself will be created later in Renderer, once per
    // frame-in-flight, but the layout describing its shape must exist
    // before the pipeline can be built.
    class Vulkan_Pipeline
    {
        VkDevice              device_handle;
        VkDescriptorSetLayout descriptor_set_layout;
        VkPipelineLayout      pipeline_layout;
        VkPipeline            pipeline;

    public:

        // Creates the pipeline from a Pipeline_Config built against
        // the given render pass. Shader modules are created internally
        // and destroyed immediately after pipeline creation.
        Vulkan_Pipeline(const Vulkan_Device& _device,
                        const Vulkan_Render_Pass& _render_pass,
                        Pipeline_Config           _config);
            

        ~Vulkan_Pipeline();

        Vulkan_Pipeline(const Vulkan_Pipeline&) = delete;
        Vulkan_Pipeline& operator=(const Vulkan_Pipeline&) = delete;

        Vulkan_Pipeline(Vulkan_Pipeline&& _other) noexcept;
        Vulkan_Pipeline& operator=(Vulkan_Pipeline&& _other) noexcept;

        // The pipeline itself — bound with vkCmdBindPipeline before
        // issuing draw calls.
        VkPipeline Get_handle() const;

        // The pipeline layout — needed when binding descriptor sets
        // (vkCmdBindDescriptorSets) and for any push constants.
        VkPipelineLayout Get_layout_handle() const;

        // The descriptor set layout describing the MVP uniform buffer's
        // shape — needed by whoever creates the actual descriptor sets
        // (allocated from a descriptor pool, holding the real uniform
        // buffers, one per frame-in-flight) later in Renderer.
        VkDescriptorSetLayout Get_descriptor_set_layout() const;

    private:

        void Destroy();
        void Create_descriptor_set_layout();
        void Create_pipeline_layout();
        VkShaderModule Create_shader_module(const std::string& _spv_file_path) const;
    };

} // namespace Renderer