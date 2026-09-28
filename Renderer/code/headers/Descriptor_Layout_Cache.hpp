#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Device.hpp>
#include <Descriptor_Sets.hpp>

#include <array>

namespace Renderer_System
{
    // Set 1 has one layout per pipeline kind (Pipeline_Kind):
    //   compute  - the resources written by compute passes
    //              (Binding_Per_Pass), visible to the compute stage only;
    //   graphics - the input attachments of the composite subpass
    //              (Binding_Graphics_Pass), visible to the fragment stage
    //              only.
    //
    // Set 2 holds the global material table (Binding_Per_Material),
    // visible to the fragment stage (material sampling) and the compute
    // stage (passes that read materials, e.g. GPU culling), and the mesh
    // table, visible to the compute stage (culling) and the vertex stage
    // (bounding volume debug draw).
    //
    // The storage buffers each stage can see across sets 0-2 are counted
    // while the layouts are built; exceeding Required_Storage_Buffers, the
    // limit device selection demanded, throws std::logic_error. The
    // graphics set 1 holds no storage buffer, so the count of the compute
    // contract is the larger one.
    class Descriptor_Layout_Cache
    {
        VkDevice device_handle;

        // Set layouts of each contract, in set order, ready for
        // vkCreatePipelineLayout. Sets 0, 2 and 3 are the same handles in
        // both; set 3 is borrowed from the Bindless_Registry.
        std::array<VkDescriptorSetLayout, Descriptor_Set::Count> compute_layouts{};
        std::array<VkDescriptorSetLayout, Descriptor_Set::Count> graphics_layouts{};

        // Storage buffer bindings per stage (vertex, fragment, compute) and
        // in total, accumulated by Record_bindings.
        std::array<uint32_t, 3> storage_buffers_per_stage{};
        uint32_t                storage_buffers_total = 0;

    public:

        Descriptor_Layout_Cache(const Vulkan_Device& _device,
            VkDescriptorSetLayout _bindless_layout);
        ~Descriptor_Layout_Cache();

        Descriptor_Layout_Cache(const Descriptor_Layout_Cache&) = delete;
        Descriptor_Layout_Cache& operator=(const Descriptor_Layout_Cache&) = delete;
        Descriptor_Layout_Cache(Descriptor_Layout_Cache&&) = delete;
        Descriptor_Layout_Cache& operator=(Descriptor_Layout_Cache&&) = delete;

        // Layout of set _set of the _kind contract, to allocate sets of it
        // (vkAllocateDescriptorSets). Sets 0, 2 and 3 return the same
        // layout for both kinds.
        VkDescriptorSetLayout Get(uint32_t _set, Pipeline_Kind _kind) const;

        // The four layouts of the _kind contract, in set order, for
        // vkCreatePipelineLayout.
        const VkDescriptorSetLayout* Data(Pipeline_Kind _kind) const;

    private:

        VkDescriptorSetLayout Create_per_frame_layout();
        VkDescriptorSetLayout Create_per_pass_layout();
        VkDescriptorSetLayout Create_graphics_pass_layout();
        VkDescriptorSetLayout Create_per_material_layout();

        // Destroys every layout this cache created (not the borrowed
        // bindless one). Safe on a partially built cache.
        void Destroy_owned_layouts();

        // Adds the storage buffer bindings of one set layout (sets 0-2) to
        // the counters.
        void Record_bindings(const VkDescriptorSetLayoutBinding* _bindings, uint32_t _count);

        // Throws std::logic_error if the counters exceed
        // Required_Storage_Buffers.
        void Check_storage_buffer_budget() const;
    };
}
