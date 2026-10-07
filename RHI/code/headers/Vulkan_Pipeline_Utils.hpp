#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Handles.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace Renderer_System
{
    // Vulkan_Pipeline_Utils: what graphics and compute pipelines have in
    // common when they are built. Vulkan_Pipeline and Vulkan_Compute_Pipeline
    // used to carry their own copy of each of these.
    namespace Vulkan_Pipeline_Utils
    {

        // Reads the SPIR-V file at _spv_file_path and creates a shader module
        // from it. The module is owned from the moment it exists, so it is
        // destroyed on every exit path of the caller, including an exception
        // thrown while the next module or the pipeline is created.
        //
        // Throws std::runtime_error if the file cannot be read, is empty, or
        // its size is not a multiple of 4 (not SPIR-V), and Vulkan_Error if
        // the creation fails.
        Unique_Shader_Module Create_shader_module(VkDevice _device, const std::string& _spv_file_path);

        // The creation feedback of one pipeline (core in Vulkan 1.3): the
        // only way to know whether the pipeline cache was actually hit
        // instead of assuming it.
        //
        //   Creation_Feedback feedback(stage_count);
        //   pipeline_info.pNext = feedback.Get_create_info();   // BEFORE creation
        //   vkCreate*Pipelines(...);
        //   feedback.Log("Graphics pipeline");
        //
        // The create info points into this object, so it is neither copyable
        // nor movable and must outlive the creation call.
        class Creation_Feedback
        {
        public:

            // The most shader stages a pipeline of the engine has (vertex and
            // fragment).
            static constexpr uint32_t MAX_STAGES = 2;

            // _stage_count: the number of shader stages of the pipeline.
            // Throws std::invalid_argument above MAX_STAGES.
            explicit Creation_Feedback(uint32_t _stage_count);

            Creation_Feedback(const Creation_Feedback&) = delete;
            Creation_Feedback& operator=(const Creation_Feedback&) = delete;
            Creation_Feedback(Creation_Feedback&&) = delete;
            Creation_Feedback& operator=(Creation_Feedback&&) = delete;

            // For the pNext of the create info, chained before the creation.
            const VkPipelineCreationFeedbackCreateInfo* Get_create_info() const { return &create_info; }

            // Prints "[Vulkan_Pipeline] <_label> created - CACHE HIT | cache
            // miss (compiled), X ms." once the pipeline was created. When
            // the driver did not fill the feedback in, says so instead: every
            // other bit is meaningless without VALID_BIT.
            void Log(const std::string& _label) const;

        private:

            VkPipelineCreationFeedback                          pipeline_feedback{};
            std::array<VkPipelineCreationFeedback, MAX_STAGES>  stage_feedbacks{};
            VkPipelineCreationFeedbackCreateInfo                create_info{};
        };

    }
}
