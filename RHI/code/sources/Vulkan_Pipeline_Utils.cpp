#include <Vulkan_Pipeline_Utils.hpp>
#include <Vulkan_Utils.hpp>
#include <Filesystem.hpp>

#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Renderer_System
{
    namespace Vulkan_Pipeline_Utils
    {

        // ---------- Create_shader_module ----------
        Unique_Shader_Module Create_shader_module(VkDevice _device, const std::string& _spv_file_path)
        {
            const std::optional<std::vector<uint8_t>> shader_file = Platform::Filesystem::Read_binary_file(_spv_file_path);

            if (!shader_file) {
                throw std::runtime_error(
                    "Failed to read shader file: " + _spv_file_path
                );
            }

            const std::vector<uint8_t>& shader_code = *shader_file;

            if (shader_code.empty()) {
                throw std::runtime_error(
                    "Shader file is empty: " + _spv_file_path
                );
            }

            if (shader_code.size() % sizeof(uint32_t) != 0) {
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

            // The output of a failed creation is undefined: the owner is
            // built only from the handle of a successful one.
            VkShaderModule shader_module = VK_NULL_HANDLE;
            VK_CHECK(vkCreateShaderModule(_device, &create_info, nullptr, &shader_module),
                ("Failed to create shader module from '" + _spv_file_path + "'").c_str());

            return Unique_Shader_Module(_device, shader_module);
        }

        // ---------- Creation_Feedback ----------
        Creation_Feedback::Creation_Feedback(uint32_t _stage_count)
        {
            if (_stage_count > MAX_STAGES)
                throw std::invalid_argument("Creation_Feedback: a pipeline has at most " + std::to_string(MAX_STAGES) + " shader stages");

            create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
            create_info.pPipelineCreationFeedback = &pipeline_feedback;
            create_info.pipelineStageCreationFeedbackCount = _stage_count;
            create_info.pPipelineStageCreationFeedbacks = stage_feedbacks.data();
        }

        void Creation_Feedback::Log(const std::string& _label) const
        {
            // VALID_BIT first: if the driver didn't fill the feedback in,
            // every other bit in it is meaningless.
            if (pipeline_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)
            {
                const bool cache_hit = (pipeline_feedback.flags &
                    VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;

                std::cout << "[Vulkan_Pipeline] " << _label << " created - "
                    << (cache_hit ? "CACHE HIT" : "cache miss (compiled)")
                    << ", " << (pipeline_feedback.duration / 1000000.0)
                    << " ms.\n";
            }
            else
            {
                std::cout << "[Vulkan_Pipeline] " << _label << " created "
                    "(driver reported no creation feedback).\n";
            }
        }

    }
}
