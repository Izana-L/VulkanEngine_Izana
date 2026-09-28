#pragma once

#include <vulkan/vulkan.h>

#include <Descriptor_Layout_Cache.hpp>
#include <Vulkan_Device.hpp>

namespace Renderer_System
{

    // Pipeline_Layout: the VkPipelineLayout of one pipeline contract,
    // composed from the set layouts of Descriptor_Layout_Cache.
    //
    // Both used to live inside Vulkan_Pipeline, which was correct while
    // there was exactly one pipeline. With a registry building N of them
    // that would mean N identical copies of both objects, so they are
    // hoisted here and created once.
    //
    // The graphics contract, shared by every graphics pipeline:
    //   set 0 : per-frame UBO, lights, objects, cluster lists (Descriptor_Layout_Cache)
    //   set 1 : input attachments of the composite subpass (Binding_Graphics_Pass)
    //   set 2 : material and mesh tables
    //   set 3 : global bindless texture array (Bindless_Registry)
    //   no push constant range: graphics shaders read everything per draw
    //   from the object, material and mesh tables (sets 0 and 2)
    //
    // Compute pipelines use a second instance: the same set layouts
    // except set 1, the compute pass resources (Binding_Per_Pass), and
    // their own push constant range (compute stage only). The set numbering
    // is identical, so sets 0, 2 and 3 are the same VkDescriptorSet handles
    // at both bind points. A separate instance is required anyway because
    // vkCmdPushConstants must use exactly the stage flags of the range it
    // updates.
    //
    // The day the number of distinct contracts grows (a shadow pass with no
    // bindless set, several compute push blocks), this becomes a small
    // cache keyed by the contract — same shape as Pipeline_Registry.
    class Pipeline_Layout
    {
        VkDevice         device_handle;
        VkPipelineLayout pipeline_layout;              // does not own the set layouts
    public:
        // The graphics contract. The set layouts belong to
        // Descriptor_Layout_Cache. This class only composes the
        // VkPipelineLayout from the four of them, without a push constant
        // range.
        Pipeline_Layout(const Vulkan_Device& _device, const Descriptor_Layout_Cache& _layouts);

        // The four set layouts of the _kind contract, with a single push
        // constant range of _push_constant_size bytes at offset 0, visible
        // to _push_constant_stages. A size of 0 declares no push constant
        // range.
        //
        // _push_constant_size must be a multiple of 4 and no larger than
        // maxPushConstantsSize (128 bytes is the guaranteed minimum).
        // _push_constant_stages must be non-zero when _push_constant_size
        // is non-zero.
        Pipeline_Layout(const Vulkan_Device& _device, const Descriptor_Layout_Cache& _layouts, Pipeline_Kind _kind,
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
