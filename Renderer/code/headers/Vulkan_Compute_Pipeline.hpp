#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>

#include <string>

namespace Renderer_System
{

    // Vulkan_Compute_Pipeline: owns a VkPipeline created with
    // vkCreateComputePipelines from a single compute shader.
    //
    // Kept separate from Vulkan_Pipeline and Pipeline_Config on purpose:
    // a compute pipeline has no vertex input, rasterization, depth or
    // blend state, and no render pass. Its only inputs are the SPIR-V of
    // the .comp shader and the pipeline layout it is built against.
    //
    // Not registered in Pipeline_Registry: the engine holds a small, fixed
    // number of compute pipelines, stored as direct members of the
    // Renderer. The VkPipelineCache of Pipeline_Cache is still used, so a
    // compute pipeline benefits from the on-disk cache like any graphics
    // pipeline.
    //
    // Binding rule: bound with VK_PIPELINE_BIND_POINT_COMPUTE, together
    // with its descriptor sets at the same bind point. State bound at
    // VK_PIPELINE_BIND_POINT_GRAPHICS is not visible to vkCmdDispatch.
    //
    // Not copyable; movable, like Vulkan_Pipeline.
    class Vulkan_Compute_Pipeline
    {
        VkDevice   device_handle;
        VkPipeline pipeline;

    public:

        // Creates the pipeline from the SPIR-V file at _shader_path, entry
        // point "main", against _pipeline_layout. The shader module is
        // created internally and destroyed right after pipeline creation.
        //
        // _pipeline_layout must declare a push constant range with
        // VK_SHADER_STAGE_COMPUTE_BIT if the shader reads push constants.
        //
        // Throws std::runtime_error if the file cannot be read, is not
        // valid SPIR-V, or pipeline creation fails.
        Vulkan_Compute_Pipeline(const Vulkan_Device& _device, VkPipelineCache _pipeline_cache,
            VkPipelineLayout _pipeline_layout, const std::string& _shader_path);

        ~Vulkan_Compute_Pipeline();

        Vulkan_Compute_Pipeline(const Vulkan_Compute_Pipeline&) = delete;
        Vulkan_Compute_Pipeline& operator=(const Vulkan_Compute_Pipeline&) = delete;

        Vulkan_Compute_Pipeline(Vulkan_Compute_Pipeline&& _other) noexcept;
        Vulkan_Compute_Pipeline& operator=(Vulkan_Compute_Pipeline&& _other) noexcept;

        // Bound with vkCmdBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE)
        // before vkCmdDispatch.
        VkPipeline Get_handle() const;

    private:

        void Destroy();
        VkShaderModule Create_shader_module(const std::string& _spv_file_path) const;
    };

} // namespace Renderer_System