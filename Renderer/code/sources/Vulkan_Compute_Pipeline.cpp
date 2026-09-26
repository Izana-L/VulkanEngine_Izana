#include <Vulkan_Compute_Pipeline.hpp>
#include <Vulkan_Utils.hpp>
#include <Filesystem.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace Renderer_System
{

    namespace
    {
        // Scoped owner of a VkShaderModule. The module is only needed while
        // vkCreateComputePipelines runs; this guarantees it is destroyed on
        // every exit path of the constructor, including a failed creation.
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
    Vulkan_Compute_Pipeline::Vulkan_Compute_Pipeline(const Vulkan_Device& _device, VkPipelineCache _pipeline_cache,
        VkPipelineLayout _pipeline_layout, const std::string& _shader_path)
        : device_handle(_device.Get_logical_device_handle()), pipeline(VK_NULL_HANDLE)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a compute pipeline");
        assert(_pipeline_layout != VK_NULL_HANDLE &&
            "Vulkan_Compute_Pipeline: the compute pipeline layout must be created first");
        assert(!_shader_path.empty() &&
            "Vulkan_Compute_Pipeline: _shader_path must not be empty");

        // ---------- Shader module ----------
        const Shader_Module compute_shader_module(device_handle, Create_shader_module(_shader_path));

        VkPipelineShaderStageCreateInfo compute_stage_info{};
        compute_stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute_stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute_stage_info.module = compute_shader_module.handle;
        compute_stage_info.pName = "main";

        // ---------- Creation feedback (core in Vulkan 1.3) ----------
        // Reports whether the pipeline cache was hit, same as the graphics
        // pipelines. Must be chained BEFORE creation.
        VkPipelineCreationFeedback pipeline_feedback{};
        VkPipelineCreationFeedback stage_feedback{};

        VkPipelineCreationFeedbackCreateInfo feedback_info{};
        feedback_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
        feedback_info.pPipelineCreationFeedback = &pipeline_feedback;
        feedback_info.pipelineStageCreationFeedbackCount = 1;
        feedback_info.pPipelineStageCreationFeedbacks = &stage_feedback;

        // ---------- Pipeline creation ----------
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &feedback_info;
        pipeline_info.stage = compute_stage_info;
        pipeline_info.layout = _pipeline_layout;

        // The shader module is destroyed by its guard when this constructor
        // returns or throws; the pipeline keeps no reference to it.
        VK_CHECK(vkCreateComputePipelines(device_handle, _pipeline_cache, 1, &pipeline_info, nullptr, &pipeline),
            ("Failed to create compute pipeline from '" + _shader_path + "'").c_str());

        // VALID_BIT first: without it, every other feedback bit is meaningless.
        if (pipeline_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)
        {
            const bool cache_hit = (pipeline_feedback.flags &
                VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;

            std::cout << "[Vulkan_Compute_Pipeline] Compute pipeline created ("
                << _shader_path << ") - "
                << (cache_hit ? "CACHE HIT" : "cache miss (compiled)")
                << ", " << (pipeline_feedback.duration / 1000000.0)
                << " ms.\n";
        }
        else
        {
            std::cout << "[Vulkan_Compute_Pipeline] Compute pipeline created ("
                << _shader_path << ") (driver reported no creation feedback).\n";
        }
    }

    // ---------- Destructor ----------
    Vulkan_Compute_Pipeline::~Vulkan_Compute_Pipeline()
    {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Compute_Pipeline::Destroy()
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device_handle, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }

    // ---------- Move constructor ----------
    Vulkan_Compute_Pipeline::Vulkan_Compute_Pipeline(Vulkan_Compute_Pipeline&& _other) noexcept
        : device_handle(_other.device_handle),
        pipeline(_other.pipeline)
    {
        _other.pipeline = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Compute_Pipeline& Vulkan_Compute_Pipeline::operator=(Vulkan_Compute_Pipeline&& _other) noexcept
    {
        if (this != &_other)
        {
            Destroy();

            device_handle = _other.device_handle;
            pipeline = _other.pipeline;

            _other.pipeline = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Getters ----------
    VkPipeline Vulkan_Compute_Pipeline::Get_handle() const
    {
        assert(pipeline != VK_NULL_HANDLE &&
            "Get_handle() called on a moved-from or destroyed Vulkan_Compute_Pipeline");
        return pipeline;
    }

    // ---------- Create_shader_module ----------
    VkShaderModule Vulkan_Compute_Pipeline::Create_shader_module(const std::string& _spv_file_path) const
    {
        std::vector<uint8_t> shader_code = Platform::Filesystem::Read_binary_file(_spv_file_path);

        if (shader_code.empty())
        {
            throw std::runtime_error(
                "Failed to read shader file or file is empty: " + _spv_file_path
            );
        }

        if (shader_code.size() % sizeof(uint32_t) != 0)
        {
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

} // namespace Renderer_System