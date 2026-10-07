#include <Vulkan_Descriptor_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Renderer_System
{
    namespace Vulkan_Descriptor_Utils
    {

        namespace
        {
            bool Is_buffer_type(VkDescriptorType _type)
            {
                return _type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || _type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                       _type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || _type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            }

            bool Is_image_type(VkDescriptorType _type)
            {
                return _type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || _type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                       _type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            }

            bool Is_sampler_type(VkDescriptorType _type)
            {
                return _type == VK_DESCRIPTOR_TYPE_SAMPLER;
            }
        }

        // ---------- Find_binding ----------
        const Layout_Binding& Find_binding(std::span<const Layout_Binding> _bindings, uint32_t _binding)
        {
            for (const Layout_Binding& entry : _bindings)
            {
                if (entry.binding == _binding)
                    return entry;
            }

            throw std::out_of_range("Vulkan_Descriptor_Utils: the layout has no binding " + std::to_string(_binding));
        }

        // ---------- To_vk_bindings ----------
        std::vector<VkDescriptorSetLayoutBinding> To_vk_bindings(std::span<const Layout_Binding> _bindings)
        {
            std::vector<VkDescriptorSetLayoutBinding> converted;
            converted.reserve(_bindings.size());

            for (const Layout_Binding& entry : _bindings)
            {
                VkDescriptorSetLayoutBinding binding{};
                binding.binding = entry.binding;
                binding.descriptorType = entry.type;
                binding.descriptorCount = entry.count;
                binding.stageFlags = entry.stages;

                converted.push_back(binding);
            }

            return converted;
        }

        // ---------- Create_set_layout ----------
        VkDescriptorSetLayout Create_set_layout(VkDevice _device, std::span<const Layout_Binding> _bindings, const char* _what)
        {
            const std::vector<VkDescriptorSetLayoutBinding> bindings = To_vk_bindings(_bindings);

            VkDescriptorSetLayoutCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();

            // The output of a failed creation is undefined and never returned.
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            VK_CHECK(vkCreateDescriptorSetLayout(_device, &info, nullptr, &layout), _what);

            return layout;
        }

        // ---------- Allocate_set / Allocate_sets ----------
        VkDescriptorSet Allocate_set(VkDevice _device, VkDescriptorPool _pool, VkDescriptorSetLayout _layout, const char* _what)
        {
            VkDescriptorSetAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            info.descriptorPool = _pool;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &_layout;

            VkDescriptorSet set = VK_NULL_HANDLE;
            VK_CHECK(vkAllocateDescriptorSets(_device, &info, &set), _what);

            return set;
        }

        void Allocate_sets(VkDevice _device, VkDescriptorPool _pool, VkDescriptorSetLayout _layout,
                           std::span<VkDescriptorSet> _out_sets, const char* _what)
        {
            if (_out_sets.empty())
                return;

            const std::vector<VkDescriptorSetLayout> layouts(_out_sets.size(), _layout);
            std::vector<VkDescriptorSet>             allocated(_out_sets.size(), VK_NULL_HANDLE);

            VkDescriptorSetAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            info.descriptorPool = _pool;
            info.descriptorSetCount = static_cast<uint32_t>(layouts.size());
            info.pSetLayouts = layouts.data();

            VK_CHECK(vkAllocateDescriptorSets(_device, &info, allocated.data()), _what);

            // The output of a failed allocation is undefined: the caller's
            // handles change only after a successful one.
            std::copy(allocated.begin(), allocated.end(), _out_sets.begin());
        }

        // ---------- Pool_Builder ----------
        Pool_Builder& Pool_Builder::Add_sets(std::span<const Layout_Binding> _bindings, uint32_t _copies)
        {
            if (_copies == 0)
                throw std::invalid_argument("Pool_Builder::Add_sets: _copies must be at least 1");

            for (const Layout_Binding& entry : _bindings)
            {
                // A binding without descriptors is a reserved number: the
                // sets hold nothing for it, and a pool size of zero is
                // invalid.
                if (entry.count == 0)
                    continue;

                const uint32_t descriptors = entry.count * _copies;

                bool merged = false;

                for (VkDescriptorPoolSize& size : sizes)
                {
                    if (size.type == entry.type)
                    {
                        size.descriptorCount += descriptors;
                        merged = true;
                        break;
                    }
                }

                if (!merged)
                    sizes.push_back({ entry.type, descriptors });
            }

            max_sets += _copies;

            return *this;
        }

        VkDescriptorPool Pool_Builder::Create(VkDevice _device, VkDescriptorPoolCreateFlags _flags, const char* _what) const
        {
            if (max_sets == 0)
                throw std::logic_error("Pool_Builder::Create: no set was added to the pool");

            VkDescriptorPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.flags = _flags;
            info.maxSets = max_sets;
            info.poolSizeCount = static_cast<uint32_t>(sizes.size());
            info.pPoolSizes = sizes.data();

            VkDescriptorPool pool = VK_NULL_HANDLE;
            VK_CHECK(vkCreateDescriptorPool(_device, &info, nullptr, &pool), _what);

            return pool;
        }

        uint32_t Pool_Builder::Get_descriptor_count(VkDescriptorType _type) const
        {
            for (const VkDescriptorPoolSize& size : sizes)
            {
                if (size.type == _type)
                    return size.descriptorCount;
            }

            return 0;
        }

        // ---------- Descriptor_Writer ----------
        const Layout_Binding& Descriptor_Writer::Find_for(uint32_t _binding, bool (*_accepts)(VkDescriptorType),
                                                          const char* _expected) const
        {
            const Layout_Binding& entry = Find_binding(layout, _binding);

            if (!_accepts(entry.type))
            {
                throw std::logic_error("Descriptor_Writer: binding " + std::to_string(_binding) + " of the layout is not " + _expected +
                                       " (descriptor type " + std::to_string(static_cast<int>(entry.type)) + ")");
            }

            return entry;
        }

        uint32_t Descriptor_Writer::Add(VkDescriptorSet _set, const Layout_Binding& _binding, uint32_t _array_element)
        {
            if (_set == VK_NULL_HANDLE)
                throw std::invalid_argument("Descriptor_Writer: null descriptor set for binding " + std::to_string(_binding.binding));

            if (count >= MAX_WRITES)
                throw std::length_error("Descriptor_Writer: more than " + std::to_string(MAX_WRITES) + " writes before Update");

            if (_array_element >= _binding.count)
            {
                throw std::out_of_range("Descriptor_Writer: element " + std::to_string(_array_element) + " is past the end of binding " +
                                        std::to_string(_binding.binding) + " (" + std::to_string(_binding.count) + " descriptors)");
            }

            VkWriteDescriptorSet& write = writes[count];
            write = VkWriteDescriptorSet{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = _set;
            write.dstBinding = _binding.binding;
            write.dstArrayElement = _array_element;
            write.descriptorType = _binding.type;
            write.descriptorCount = 1;

            return count++;
        }

        Descriptor_Writer& Descriptor_Writer::Write_buffer(VkDescriptorSet _set, uint32_t _binding,
                                                           const Vulkan_Buffer_Utils::Buffer_Allocation& _buffer)
        {
            return Write_buffer(_set, _binding, _buffer.buffer, _buffer.size);
        }

        Descriptor_Writer& Descriptor_Writer::Write_buffer(VkDescriptorSet _set, uint32_t _binding, VkBuffer _buffer, VkDeviceSize _range)
        {
            if (_buffer == VK_NULL_HANDLE)
                throw std::invalid_argument("Descriptor_Writer::Write_buffer: null buffer for binding " + std::to_string(_binding));

            const uint32_t index = Add(_set, Find_for(_binding, Is_buffer_type, "a buffer"), 0);

            buffer_infos[index].buffer = _buffer;
            buffer_infos[index].offset = 0;
            buffer_infos[index].range = _range;
            writes[index].pBufferInfo = &buffer_infos[index];

            return *this;
        }

        Descriptor_Writer& Descriptor_Writer::Write_image(VkDescriptorSet _set, uint32_t _binding, VkImageView _view,
                                                          VkImageLayout _layout, uint32_t _array_element)
        {
            if (_view == VK_NULL_HANDLE)
                throw std::invalid_argument("Descriptor_Writer::Write_image: null image view for binding " + std::to_string(_binding));

            const uint32_t index = Add(_set, Find_for(_binding, Is_image_type, "an image"), _array_element);

            // No sampler: these descriptor types read or write the view
            // alone (a sampler is a separate binding or a separate array).
            image_infos[index].sampler = VK_NULL_HANDLE;
            image_infos[index].imageView = _view;
            image_infos[index].imageLayout = _layout;
            writes[index].pImageInfo = &image_infos[index];

            return *this;
        }

        Descriptor_Writer& Descriptor_Writer::Write_sampler(VkDescriptorSet _set, uint32_t _binding, VkSampler _sampler,
                                                            uint32_t _array_element)
        {
            if (_sampler == VK_NULL_HANDLE)
                throw std::invalid_argument("Descriptor_Writer::Write_sampler: null sampler for binding " + std::to_string(_binding));

            const uint32_t index = Add(_set, Find_for(_binding, Is_sampler_type, "a sampler"), _array_element);

            // A SAMPLER descriptor only reads the sampler field; imageView
            // and imageLayout are ignored.
            image_infos[index].sampler = _sampler;
            image_infos[index].imageView = VK_NULL_HANDLE;
            image_infos[index].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            writes[index].pImageInfo = &image_infos[index];

            return *this;
        }

        void Descriptor_Writer::Update(VkDevice _device) noexcept
        {
            if (count == 0)
                return;

            vkUpdateDescriptorSets(_device, count, writes.data(), 0, nullptr);

            count = 0;
        }

    } // namespace Vulkan_Descriptor_Utils

} // namespace Renderer_System
