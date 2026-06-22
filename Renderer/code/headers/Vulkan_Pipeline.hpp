#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Render_Pass.hpp>

#include <string>

namespace Renderer {

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
    // (intended for Model/View/Projection matrices) - the actual uniform
    // buffer itself will be created later in Renderer, once per
    // frame-in-flight, but the layout describing its shape must exist
    // before the pipeline can be built.
    class Vulkan_Pipeline 
    {
        VkDevice device_handle;

        VkDescriptorSetLayout descriptor_set_layout;
        VkPipelineLayout pipeline_layout;
        VkPipeline pipeline;

    public:
        // Creates the pipeline using the given vertex/fragment shader
        // SPIR-V files (already compiled .spv, loaded via
        // Platform::Filesystem), built against the given render pass.
        Vulkan_Pipeline(
            const Vulkan_Device& _device,
            const Vulkan_Render_Pass& _render_pass,
            const std::string& _vertex_shader_path,
            const std::string& _fragment_shader_path
        );

        ~Vulkan_Pipeline();

        Vulkan_Pipeline(const Vulkan_Pipeline&) = delete;
        Vulkan_Pipeline& operator=(const Vulkan_Pipeline&) = delete;

        Vulkan_Pipeline(Vulkan_Pipeline&& _other) noexcept;
        Vulkan_Pipeline& operator=(Vulkan_Pipeline&& _other) noexcept;

        // The pipeline itself - bound with vkCmdBindPipeline before
        // issuing draw calls.
        VkPipeline Get_handle() const;

        // The pipeline layout - needed when binding descriptor sets
        // (vkCmdBindDescriptorSets) and for any push constants.
        VkPipelineLayout Get_layout_handle() const;

        // The descriptor set layout describing the MVP uniform buffer's
        // shape - needed by whoever creates the actual descriptor sets
        // (allocated from a descriptor pool, holding the real uniform
        // buffers, one per frame-in-flight) later in Renderer.
        VkDescriptorSetLayout Get_descriptor_set_layout() const;

    private:
        // Destroys the pipeline, its layout, and the descriptor set
        // layout, in reverse order of creation. Shared by destructor and
        // move assignment.
        void Destroy();

        // Creates the descriptor set layout describing a single uniform
        // buffer binding (for MVP matrices), visible to the vertex shader.
        void Create_descriptor_set_layout();

        // Creates the pipeline layout, referencing the descriptor set
        // layout created above.
        void Create_pipeline_layout();

        // Loads a compiled SPIR-V file from disk and wraps it in a
        // VkShaderModule.
        VkShaderModule Create_shader_module(const std::string& _spv_file_path) const;

       
    };

}