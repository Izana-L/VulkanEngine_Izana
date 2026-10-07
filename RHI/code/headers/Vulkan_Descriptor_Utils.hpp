#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Buffer_Utils.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Renderer_System
{
    // Vulkan_Descriptor_Utils: layouts, pools, sets and writes of descriptor
    // sets, driven by ONE description of each layout.
    //
    // A set layout is described once, as a table of Layout_Binding. The same
    // table creates the VkDescriptorSetLayout, sizes the pool the sets are
    // allocated from (Pool_Builder) and tells a Descriptor_Writer the type
    // of every descriptor it writes, so the layout, the pool and the writes
    // cannot disagree about how many descriptors of which type a set holds.
    namespace Vulkan_Descriptor_Utils
    {

        // One binding of a set layout.
        struct Layout_Binding
        {
            uint32_t           binding = 0;
            VkDescriptorType   type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            VkShaderStageFlags stages = 0;

            // Array size of the binding (1 for a single descriptor).
            uint32_t           count = 1;
        };

        // The entry of _bindings for binding number _binding. Throws
        // std::out_of_range when the table has none.
        const Layout_Binding& Find_binding(std::span<const Layout_Binding> _bindings, uint32_t _binding);

        // The bindings as the create info of a layout takes them.
        std::vector<VkDescriptorSetLayoutBinding> To_vk_bindings(std::span<const Layout_Binding> _bindings);

        // Creates the layout of _bindings, without flags. _what names the
        // layout in the message of the Vulkan_Error thrown on failure.
        VkDescriptorSetLayout Create_set_layout(VkDevice _device, std::span<const Layout_Binding> _bindings, const char* _what);

        // Allocates one set of _layout from _pool. Throws Vulkan_Error
        // (message starting with _what) when the pool cannot provide it.
        VkDescriptorSet Allocate_set(VkDevice _device, VkDescriptorPool _pool, VkDescriptorSetLayout _layout, const char* _what);

        // Allocates _out_sets.size() sets of the same _layout in a single
        // call. _out_sets is written only when the allocation succeeded.
        void Allocate_sets(VkDevice _device, VkDescriptorPool _pool, VkDescriptorSetLayout _layout,
                           std::span<VkDescriptorSet> _out_sets, const char* _what);

        // Sizes a descriptor pool from the layouts of the sets it will hand
        // out: every Add_sets call reserves _copies sets of a layout, and
        // the pool gets room for exactly the descriptors of those sets, per
        // type, and for that many sets.
        class Pool_Builder
        {
        public:

            // Reserves _copies sets (at least one) with the layout _bindings.
            // A binding with count 0 (a reserved number) adds no descriptors.
            Pool_Builder& Add_sets(std::span<const Layout_Binding> _bindings, uint32_t _copies = 1);

            // Creates the pool. _flags: VkDescriptorPoolCreateFlags (for
            // example UPDATE_AFTER_BIND). Throws Vulkan_Error with _what on
            // failure, or std::logic_error when nothing was added.
            VkDescriptorPool Create(VkDevice _device, VkDescriptorPoolCreateFlags _flags, const char* _what) const;

            // What the pool will be created with.
            uint32_t Get_max_sets() const { return max_sets; }
            uint32_t Get_descriptor_count(VkDescriptorType _type) const;

        private:

            std::vector<VkDescriptorPoolSize> sizes;
            uint32_t                          max_sets = 0;
        };

        // Collects writes to the sets of one layout and applies them in a
        // single vkUpdateDescriptorSets call. The descriptor type of every
        // write comes from the layout table the writer was built with, and
        // a write that does not fit that type (a buffer written to an image
        // binding, an array element past the end of the binding) or that
        // names no object (a null set, buffer, view or sampler) throws
        // std::logic_error, std::out_of_range or std::invalid_argument
        // instead of reaching the driver.
        //
        // No heap: the writes live inside the writer (up to MAX_WRITES per
        // Update), so writing one descriptor, as the bindless registry does
        // for every texture it registers, cannot fail for lack of memory.
        // Not copyable or movable: the write structures point into it.
        //
        // Every set written must follow the layout of the table, and the
        // table must outlive the writer.
        class Descriptor_Writer
        {
        public:

            static constexpr uint32_t MAX_WRITES = 16;

            explicit Descriptor_Writer(std::span<const Layout_Binding> _layout) noexcept
                : layout(_layout)
            {
            }

            Descriptor_Writer(const Descriptor_Writer&) = delete;
            Descriptor_Writer& operator=(const Descriptor_Writer&) = delete;
            Descriptor_Writer(Descriptor_Writer&&) = delete;
            Descriptor_Writer& operator=(Descriptor_Writer&&) = delete;

            // A whole buffer: offset 0, range _buffer.size.
            Descriptor_Writer& Write_buffer(VkDescriptorSet _set, uint32_t _binding,
                                            const Vulkan_Buffer_Utils::Buffer_Allocation& _buffer);

            // The first _range bytes of _buffer.
            Descriptor_Writer& Write_buffer(VkDescriptorSet _set, uint32_t _binding, VkBuffer _buffer, VkDeviceSize _range);

            // A sampled image, a storage image or an input attachment:
            // _view in _layout, element _array_element of the binding.
            Descriptor_Writer& Write_image(VkDescriptorSet _set, uint32_t _binding, VkImageView _view,
                                           VkImageLayout _layout, uint32_t _array_element = 0);

            // A sampler, element _array_element of the binding.
            Descriptor_Writer& Write_sampler(VkDescriptorSet _set, uint32_t _binding, VkSampler _sampler,
                                             uint32_t _array_element = 0);

            // Applies the writes collected so far and forgets them.
            void Update(VkDevice _device) noexcept;

        private:

            // Starts the next write, for _binding of _set, and returns its
            // index: the caller fills buffer_infos / image_infos at that
            // index and points the write at it. Throws std::length_error
            // when MAX_WRITES writes are already pending, and
            // std::out_of_range when _array_element is past the end of the
            // binding.
            uint32_t Add(VkDescriptorSet _set, const Layout_Binding& _binding, uint32_t _array_element);

            // The entry for _binding. Throws std::logic_error unless its
            // type satisfies _accepts; _expected ("a buffer"...) names what
            // the write needed, for the message.
            const Layout_Binding& Find_for(uint32_t _binding, bool (*_accepts)(VkDescriptorType), const char* _expected) const;

            std::span<const Layout_Binding>                 layout;
            uint32_t                                        count = 0;

            std::array<VkWriteDescriptorSet, MAX_WRITES>    writes{};
            std::array<VkDescriptorBufferInfo, MAX_WRITES>  buffer_infos{};
            std::array<VkDescriptorImageInfo, MAX_WRITES>   image_infos{};
        };

    } // namespace Vulkan_Descriptor_Utils

} // namespace Renderer_System
