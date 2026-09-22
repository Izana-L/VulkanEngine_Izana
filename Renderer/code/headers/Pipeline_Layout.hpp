#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Descriptor_Layout_Cache.hpp>
#include <Vulkan_Device.hpp>

namespace Renderer_System
{

    // Pipeline_Layout: the descriptor set layout (set 0) and the
    // VkPipelineLayout that EVERY pipeline in this engine shares.
    //
    // Both used to live inside Vulkan_Pipeline, which was correct while
    // there was exactly one pipeline. With a registry building N of them
    // that would mean N identical copies of both objects, so they are
    // hoisted here and created once.
    //
    // The layout contract, shared by every pipeline:
    //   set 0 : per-frame UBO + light buffer (Descriptor_Layout_Cache)
    //   set 1 : per-pass, reserved (empty layout)
    //   set 2 : per-material, reserved (empty layout)
    //   set 3 : global bindless texture array (Bindless_Registry)
    //   push constant : Push_Constants (Frame_Data.hpp), 96 bytes,
    //                   vertex + fragment stages
    //
    // The day a pipeline needs a different contract (a compute pass, a
    // shadow pass with no bindless set), this stops being one shared object
    // and becomes a small cache keyed by the contract — same shape as
    // Pipeline_Registry. Not today: every pipeline here is the same shape.
    class Pipeline_Layout
    {
        VkDevice         device_handle;
        VkPipelineLayout pipeline_layout;              // does not own the set layouts
    public:
        // The set layouts belong to Descriptor_Layout_Cache. This class only
        // composes the VkPipelineLayout from the four of them and adds the
        // push constant range (Push_Constants in Frame_Data.hpp, both stages).
        Pipeline_Layout(const Vulkan_Device& _device, const Descriptor_Layout_Cache& _layouts);
        ~Pipeline_Layout();

        Pipeline_Layout(const Pipeline_Layout&) = delete;
        Pipeline_Layout& operator=(const Pipeline_Layout&) = delete;
        Pipeline_Layout(Pipeline_Layout&&) = delete;
        Pipeline_Layout& operator=(Pipeline_Layout&&) = delete;

        VkPipelineLayout Get_handle() const { return pipeline_layout; }
        // Whoever needs a set layout asks the cache, which owns them.
    };

} // namespace Renderer_System