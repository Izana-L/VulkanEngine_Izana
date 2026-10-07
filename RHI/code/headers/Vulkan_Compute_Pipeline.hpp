#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Handles.hpp>

#include <string>
#include <type_traits>

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
    // The handle is owned by a Unique_Pipeline, like Vulkan_Pipeline's.
    // Not copyable; movable, like Vulkan_Pipeline.
    class Vulkan_Compute_Pipeline
    {
        Unique_Pipeline pipeline;

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

        ~Vulkan_Compute_Pipeline() = default;

        Vulkan_Compute_Pipeline(const Vulkan_Compute_Pipeline&) = delete;
        Vulkan_Compute_Pipeline& operator=(const Vulkan_Compute_Pipeline&) = delete;

        Vulkan_Compute_Pipeline(Vulkan_Compute_Pipeline&& _other) noexcept = default;
        Vulkan_Compute_Pipeline& operator=(Vulkan_Compute_Pipeline&& _other) noexcept = default;

        // Bound with vkCmdBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE)
        // before vkCmdDispatch.
        VkPipeline Get_handle() const;

        // Records a whole compute pass: binds the pipeline, sets its push
        // constants and dispatches _group_count_x * _group_count_y *
        // _group_count_z workgroups, the sequence every compute pass of the
        // engine records. The descriptor sets are bound by the caller, at
        // VK_PIPELINE_BIND_POINT_COMPUTE, against _pipeline_layout.
        //
        // _push_constants is copied at offset 0 to the compute stage range
        // of _pipeline_layout, which must hold sizeof(Push_Constants) bytes
        // for VK_SHADER_STAGE_COMPUTE_BIT exactly (the stage flags of the
        // range and of the call must match). Must be recorded outside a
        // render pass.
        template <typename Push_Constants>
        void Dispatch(VkCommandBuffer _command_buffer, VkPipelineLayout _pipeline_layout,
                      const Push_Constants& _push_constants,
                      uint32_t _group_count_x, uint32_t _group_count_y = 1, uint32_t _group_count_z = 1) const
        {
            static_assert(std::is_trivially_copyable_v<Push_Constants>, "push constants are copied byte by byte");

            vkCmdBindPipeline(_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, Get_handle());
            vkCmdPushConstants(_command_buffer, _pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(Push_Constants), &_push_constants);
            vkCmdDispatch(_command_buffer, _group_count_x, _group_count_y, _group_count_z);
        }

        // Same as Dispatch for a shader that declares no push constants:
        // binds the pipeline and dispatches. The descriptor sets are bound
        // by the caller, as for Dispatch. Must be recorded outside a render
        // pass.
        void Dispatch_groups(VkCommandBuffer _command_buffer,
                             uint32_t _group_count_x, uint32_t _group_count_y = 1, uint32_t _group_count_z = 1) const
        {
            vkCmdBindPipeline(_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, Get_handle());
            vkCmdDispatch(_command_buffer, _group_count_x, _group_count_y, _group_count_z);
        }
    };

} // namespace Renderer_System