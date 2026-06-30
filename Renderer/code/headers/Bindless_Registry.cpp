#include <Bindless_Registry.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <cassert>

namespace Renderer
{

    // ---------- Constructor ----------
    Bindless_Registry::Bindless_Registry(const Vulkan_Device& _device, uint32_t _max_textures)
        : device_handle(_device.Get_logical_device_handle()),
        max_textures(_max_textures),
        next_free_index(0),
        layout(VK_NULL_HANDLE),
        pool(VK_NULL_HANDLE),
        set(VK_NULL_HANDLE)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Bindless_Registry");
        assert(_device.Is_bindless_supported() &&
            "Bindless_Registry requires descriptor indexing features — "
            "Vulkan_Device::Is_bindless_supported() returned false. "
            "Check GPU/driver support before constructing this class.");
        assert(max_textures > 0 &&
            "Bindless_Registry: _max_textures must be greater than zero");

        Create_layout();
        Create_pool();
        Create_set();
    }

    // ---------- Destructor ----------
    Bindless_Registry::~Bindless_Registry()
    {
        // Destroying the pool implicitly frees the descriptor set
        // allocated from it — no separate vkFreeDescriptorSets call needed.
        if (pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_handle, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_handle, layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
    }

    // ---------- Register_texture ----------
    uint32_t Bindless_Registry::Register_texture(VkImageView _image_view, VkSampler _sampler)
    {
        assert(_image_view != VK_NULL_HANDLE &&
            "Register_texture() called with a null image view");
        assert(_sampler != VK_NULL_HANDLE &&
            "Register_texture() called with a null sampler");

        if (next_free_index >= max_textures) {
            throw std::runtime_error(
                "Bindless_Registry: exceeded max_textures (" +
                std::to_string(max_textures) +
                ") — increase the limit passed to the constructor."
            );
        }

        const uint32_t index = next_free_index;
        ++next_free_index;

        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = _image_view;
        image_info.sampler = _sampler;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.dstArrayElement = index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;

        // Writing a single array element while the set may already be bound
        // in a previous frame's command buffer is exactly what
        // UPDATE_AFTER_BIND on both the pool and the binding makes legal.
        vkUpdateDescriptorSets(device_handle, 1, &write, 0, nullptr);

        return index;
    }

    // ---------- Get_registered_count ----------
    uint32_t Bindless_Registry::Get_registered_count() const
    {
        return next_free_index;
    }

    // ---------- Get_layout / Get_set ----------
    VkDescriptorSetLayout Bindless_Registry::Get_layout() const
    {
        assert(layout != VK_NULL_HANDLE &&
            "Get_layout() called on a destroyed Bindless_Registry");
        return layout;
    }

    VkDescriptorSet Bindless_Registry::Get_set() const
    {
        assert(set != VK_NULL_HANDLE &&
            "Get_set() called on a destroyed Bindless_Registry");
        return set;
    }

    // ---------- Create_layout ----------
    void Bindless_Registry::Create_layout()
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = max_textures;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // Per-binding flags required for bindless:
        //   PARTIALLY_BOUND   — slots that were never written are legal to
        //                       leave in the descriptor set, as long as the
        //                       shader never reads them. Essential since most
        //                       of the 1024 slots start empty.
        //   UPDATE_AFTER_BIND — allows writing new slots (Register_texture)
        //                       even after this set has already been bound
        //                       in a previously-recorded command buffer.
        //   VARIABLE_DESCRIPTOR_COUNT is intentionally NOT used here — the
        //   array size is fixed at max_textures, simpler than a runtime-sized
        //   array and sufficient for this engine's needs.
        VkDescriptorBindingFlags binding_flags =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
        binding_flags_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        binding_flags_info.bindingCount = 1;
        binding_flags_info.pBindingFlags = &binding_flags;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &binding;

        // The set itself must also opt in to update-after-bind, in addition
        // to the per-binding flag above.
        layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout_info.pNext = &binding_flags_info;

        VkResult result = vkCreateDescriptorSetLayout(
            device_handle, &layout_info, nullptr, &layout);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Bindless_Registry: failed to create descriptor set layout: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }
    }

    // ---------- Create_pool ----------
    void Bindless_Registry::Create_pool()
    {
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = max_textures;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;

        // UPDATE_AFTER_BIND_BIT on the pool is required to allocate sets
        // from a layout that uses UPDATE_AFTER_BIND bindings.
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;

        // Only one descriptor set is ever allocated from this pool — the
        // single global bindless set.
        pool_info.maxSets = 1;

        VkResult result = vkCreateDescriptorPool(
            device_handle, &pool_info, nullptr, &pool);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Bindless_Registry: failed to create descriptor pool: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }
    }

    // ---------- Create_set ----------
    void Bindless_Registry::Create_set()
    {
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &layout;

        VkResult result = vkAllocateDescriptorSets(device_handle, &alloc_info, &set);

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Bindless_Registry: failed to allocate descriptor set: " +
                Vulkan_Utils::Vk_result_to_string(result)
            );
        }
    }

} // namespace Renderer