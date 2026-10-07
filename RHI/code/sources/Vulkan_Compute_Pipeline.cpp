#include <Vulkan_Compute_Pipeline.hpp>
#include <Vulkan_Pipeline_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <cassert>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Vulkan_Compute_Pipeline::Vulkan_Compute_Pipeline(const Vulkan_Device& _device, VkPipelineCache _pipeline_cache,
        VkPipelineLayout _pipeline_layout, const std::string& _shader_path)
        : pipeline()
    {
        const VkDevice device_handle = _device.Get_logical_device_handle();

        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a compute pipeline");
        assert(_pipeline_layout != VK_NULL_HANDLE &&
            "Vulkan_Compute_Pipeline: the compute pipeline layout must be created first");
        assert(!_shader_path.empty() &&
            "Vulkan_Compute_Pipeline: _shader_path must not be empty");

        // ---------- Shader module ----------
        // Owned by an RAII wrapper: destroyed on every exit path of the
        // constructor, including a failed creation.
        const Unique_Shader_Module compute_shader_module = Vulkan_Pipeline_Utils::Create_shader_module(device_handle, _shader_path);

        VkPipelineShaderStageCreateInfo compute_stage_info{};
        compute_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute_stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute_stage_info.module = compute_shader_module.Get();
        compute_stage_info.pName = "main";

        // ---------- Creation feedback (core in Vulkan 1.3) ----------
        // Reports whether the pipeline cache was hit, same as the graphics
        // pipelines. Must be chained BEFORE creation. Not const: the driver
        // writes the feedback into it during creation.
        Vulkan_Pipeline_Utils::Creation_Feedback feedback(1);

        // ---------- Pipeline creation ----------
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = feedback.Get_create_info();
        pipeline_info.stage = compute_stage_info;
        pipeline_info.layout = _pipeline_layout;

        // The shader module is destroyed by its wrapper when this
        // constructor returns or throws; the pipeline keeps no reference to
        // it. The output of a failed creation is undefined: the owner is
        // built only from a successful one.
        VkPipeline created = VK_NULL_HANDLE;
        VK_CHECK(vkCreateComputePipelines(device_handle, _pipeline_cache, 1, &pipeline_info, nullptr, &created),
            ("Failed to create compute pipeline from '" + _shader_path + "'").c_str());

        pipeline = Unique_Pipeline(device_handle, created);

        feedback.Log("Compute pipeline (" + _shader_path + ")");
    }

    // ---------- Getters ----------
    VkPipeline Vulkan_Compute_Pipeline::Get_handle() const
    {
        assert(pipeline &&
            "Get_handle() called on a moved-from or destroyed Vulkan_Compute_Pipeline");
        return pipeline.Get();
    }

} // namespace Renderer_System
