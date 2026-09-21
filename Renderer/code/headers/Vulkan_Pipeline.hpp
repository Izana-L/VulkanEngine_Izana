#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Render_Pass.hpp>

#include <string>

namespace Renderer_System
{

    // Pipeline_Config: all parameters needed to create a Vulkan_Pipeline.
    // Passed by value to the constructor so the pipeline interface stays
    // stable as new fields are added (e.g. blend mode, cull mode, push
    // constant ranges, extra descriptor set layouts for PBR textures).
    // Fields that don't change between pipelines use sensible defaults.
    struct Raster_State
    {
        VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
        VkFrontFace     front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        bool            depth_test_enable = true;
        bool            depth_write_enable = true;

        // Reverse-Z: the near plane maps to 1.0 and the far end to 0.0, so
        // "closer to the camera" means a GREATER depth value. Must stay in
        // sync with the 0.0 depth clear and the reversed projection matrix
        // built in Extractor.cpp.
        //
        // Becomes VK_COMPARE_OP_GREATER_OR_EQUAL once a depth prepass exists
        // (the main pass then tests for equality against the prepass).
        VkCompareOp     depth_compare_op = VK_COMPARE_OP_GREATER;
    };
    struct Pipeline_Config
    {
        // ── Shaders (required) ────────────────────────────────────────
        std::string vertex_shader_path;
        std::string fragment_shader_path;

        // Pipeline_Config IS the registry key — these two must agree with
        // Pipeline_Config_Hash, field for field.
        //
        // A field added to one but not the other fails silently, and in the
        // worst direction: two different pipelines collapse into one entry
        // (you render with the wrong state) or the same pipeline gets built
        // twice (you leak a VkPipeline). Nothing crashes. Add fields to both
        // in the same edit, always.
        bool operator==(const Pipeline_Config& _other) const
        {
            return vertex_shader_path == _other.vertex_shader_path
                && fragment_shader_path == _other.fragment_shader_path
                && polygon_mode == _other.polygon_mode
                && blend_enable == _other.blend_enable
                && src_color_blend_factor == _other.src_color_blend_factor
                && dst_color_blend_factor == _other.dst_color_blend_factor
                && color_blend_op == _other.color_blend_op
                && src_alpha_blend_factor == _other.src_alpha_blend_factor
                && dst_alpha_blend_factor == _other.dst_alpha_blend_factor
                && alpha_blend_op == _other.alpha_blend_op;
        }

        // ── Rasterization ─────────────────────────────────────────────
        // polygon_mode stays baked in: making it dynamic needs
        // VK_EXT_extended_dynamic_state3, which is NOT core in 1.3.
        VkPolygonMode   polygon_mode = VK_POLYGON_MODE_FILL;

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
        VkPipeline            pipeline;

    public:

        // Creates the pipeline from a Pipeline_Config built against
        // the given render pass. Shader modules are created internally
        // and destroyed immediately after pipeline creation.
        Vulkan_Pipeline(const Vulkan_Device& _device,const Vulkan_Render_Pass& _render_pass, VkPipelineCache _pipeline_cache,
                              VkPipelineLayout _pipeline_layout, Pipeline_Config  _config);

        ~Vulkan_Pipeline();

        Vulkan_Pipeline(const Vulkan_Pipeline&) = delete;
        Vulkan_Pipeline& operator=(const Vulkan_Pipeline&) = delete;

        Vulkan_Pipeline(Vulkan_Pipeline&& _other) noexcept;
        Vulkan_Pipeline& operator=(Vulkan_Pipeline&& _other) noexcept;

        // The pipeline itself — bound with vkCmdBindPipeline before
        // issuing draw calls.
        VkPipeline Get_handle() const;



    private:

        void Destroy();
        VkShaderModule Create_shader_module(const std::string& _spv_file_path) const;
    };

} // namespace Renderer