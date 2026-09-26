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
        // The layout contract, shared by every graphics pipeline:
    //   set 0 : per-frame UBO + light buffer (Descriptor_Layout_Cache)
    //   set 1 : per-pass, reserved (empty layout)
    //   set 2 : per-material, reserved (empty layout)
    //   set 3 : global bindless texture array (Bindless_Registry)
    //   push constant : Push_Constants (Frame_Data.hpp), 96 bytes,
    //                   vertex + fragment stages
    //
    // Compute pipelines use a second instance built with the same four set
    // layouts and their own push constant range (compute stage only). The
    // set numbering is identical, so the same VkDescriptorSet handles are
    // bound at VK_PIPELINE_BIND_POINT_COMPUTE without any new descriptor.
    // A separate instance is required because vkCmdPushConstants must use
    // exactly the stage flags of the range it updates.
    //
    // The day the number of distinct contracts grows (a shadow pass with no
    // bindless set, several compute push blocks), this becomes a small
    // cache keyed by the contract — same shape as Pipeline_Registry.
    class Pipeline_Layout
    {
        VkDevice         device_handle;
        VkPipelineLayout pipeline_layout;              // does not own the set layouts
    public:
        // The set layouts belong to Descriptor_Layout_Cache. This class only
        // composes the VkPipelineLayout from the four of them and adds the
        // push constant range (Push_Constants in Frame_Data.hpp, both stages).
        Pipeline_Layout(const Vulkan_Device& _device, const Descriptor_Layout_Cache& _layouts);

        // Same four set layouts, with a single push constant range of
        // _push_constant_size bytes at offset 0, visible to
        // _push_constant_stages. A size of 0 declares no push constant range.
        //
        // _push_constant_size must be a multiple of 4 and no larger than
        // maxPushConstantsSize (128 bytes is the guaranteed minimum).
        // _push_constant_stages must be non-zero when _push_constant_size
        // is non-zero.
        Pipeline_Layout(const Vulkan_Device& _device, const Descriptor_Layout_Cache& _layouts,
            VkShaderStageFlags _push_constant_stages, uint32_t _push_constant_size);
        ~Pipeline_Layout();

        Pipeline_Layout(const Pipeline_Layout&) = delete;
        Pipeline_Layout& operator=(const Pipeline_Layout&) = delete;
        Pipeline_Layout(Pipeline_Layout&&) = delete;
        Pipeline_Layout& operator=(Pipeline_Layout&&) = delete;

        VkPipelineLayout Get_handle() const { return pipeline_layout; }
        // Whoever needs a set layout asks the cache, which owns them.
    };

} // namespace Renderer_System