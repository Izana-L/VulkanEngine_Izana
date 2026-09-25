#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace Renderer_System
{
    // =========================================================
    // Stages that read the bindless set (set 3)
    // =========================================================
    //
    // Single source for the stages that may read textures[] and
    // samplers[], expressed in the two vocabularies Vulkan uses:
    //
    //   Bindless_Reader_Stages          - VkShaderStageFlags. Visibility of
    //                                     both bindless bindings in the
    //                                     descriptor set layout
    //                                     (Bindless_Registry::Create_layout).
    //   Bindless_Reader_Pipeline_Stages - VkPipelineStageFlags. Destination
    //                                     stage of every barrier that leaves
    //                                     an image readable through the
    //                                     bindless set (Vulkan_Image_Utils).
    //
    // Both constants must describe the same stages. A stage visible in the
    // layout but absent from the barriers can read a texture before its
    // upload is visible to it, with no validation error. The static_asserts
    // at the end of this file reject that mismatch at compile time.
    //
    // FRAGMENT - material sampling (mesh.frag).
    // COMPUTE  - compute passes that read textures through the bindless set.
    // VERTEX is excluded: no vertex shader samples textures. Adding it
    // (e.g. for displacement) means adding it to both constants.

    inline constexpr VkShaderStageFlags Bindless_Reader_Stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    inline constexpr VkPipelineStageFlags Bindless_Reader_Pipeline_Stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    // Shader stages that To_pipeline_stages translates.
    inline constexpr VkShaderStageFlags Translatable_Shader_Stages =VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    // Pipeline stages that execute the given shader stages. Bits outside
    // Translatable_Shader_Stages are ignored; the static_assert below
    // guarantees none reaches this function from the bindless constants.
    constexpr VkPipelineStageFlags To_pipeline_stages(VkShaderStageFlags _stages)
    {
        VkPipelineStageFlags pipeline_stages = 0;

        if (_stages & VK_SHADER_STAGE_VERTEX_BIT)   pipeline_stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        if (_stages & VK_SHADER_STAGE_FRAGMENT_BIT) pipeline_stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        if (_stages & VK_SHADER_STAGE_COMPUTE_BIT)  pipeline_stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

        return pipeline_stages;
    }

    static_assert((Bindless_Reader_Stages & ~Translatable_Shader_Stages) == 0,
        "Bindless_Reader_Stages contains a stage that To_pipeline_stages does not translate");

    static_assert(To_pipeline_stages(Bindless_Reader_Stages) == Bindless_Reader_Pipeline_Stages,
        "Bindless_Reader_Stages and Bindless_Reader_Pipeline_Stages describe different stages");
}