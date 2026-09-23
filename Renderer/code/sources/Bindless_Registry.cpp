#include <Bindless_Registry.hpp>
#include <Descriptor_Sets.hpp>
#include <Vulkan_Utils.hpp>

#include <array>
#include <stdexcept>
#include <string>
#include <cassert>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Bindless_Registry::Bindless_Registry(const Vulkan_Device& _device, uint32_t _max_textures, uint32_t _max_samplers)
        : device_handle(_device.Get_logical_device_handle()),
        max_textures(_max_textures),
        max_samplers(_max_samplers),
        next_free_index(0),
        layout(VK_NULL_HANDLE),
        pool(VK_NULL_HANDLE),
        set(VK_NULL_HANDLE)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Bindless_Registry");

        // Enforced in every build: writing a descriptor array with the
        // UPDATE_AFTER_BIND / PARTIALLY_BOUND flags on a device that did
        // not enable descriptor indexing is invalid usage that no layer
        // may be present to report. Vulkan_Device only selects devices
        // with the features, so this is a defence against a future change
        // to that policy, not an expected path.
        if (!_device.Is_bindless_supported())
        {
            throw std::runtime_error(
                "Bindless_Registry: the device was created without the descriptor indexing "
                "features bindless textures require");
        }

        if (max_textures == 0)
            throw std::invalid_argument("Bindless_Registry: _max_textures must be greater than zero");

        if (max_samplers == 0)
            throw std::invalid_argument("Bindless_Registry: _max_samplers must be greater than zero");

        try
        {
            Create_layout();
            Create_pool();
            Create_set();
        }
        catch (...)
        {
            // The destructor does not run for a constructor that threw.
            if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_handle, pool, nullptr);
            if (layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_handle, layout, nullptr);
            throw;
        }
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
    uint32_t Bindless_Registry::Register_texture(VkImageView _image_view)
    {
        if (_image_view == VK_NULL_HANDLE)
            throw std::invalid_argument("Bindless_Registry::Register_texture: null image view");

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

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = Binding_Bindless::Textures;
        write.dstArrayElement = index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;

        // Writing a single array element while the set may already be bound
        // in a previous frame's command buffer is exactly what
        // UPDATE_AFTER_BIND on both the pool and the binding makes legal.
        vkUpdateDescriptorSets(device_handle, 1, &write, 0, nullptr);

        return index;
    }

    // ---------- Set_sampler ----------
    void Bindless_Registry::Set_sampler(uint32_t _index, VkSampler _sampler)
    {
        if (_sampler == VK_NULL_HANDLE)
            throw std::invalid_argument("Bindless_Registry::Set_sampler: null sampler");

        if (_index >= max_samplers) {
            throw std::invalid_argument(
                "Bindless_Registry::Set_sampler: index " + std::to_string(_index) +
                " is outside the sampler array (" + std::to_string(max_samplers) + " slots)"
            );
        }

        // A SAMPLER descriptor only reads the sampler field; imageView and
        // imageLayout are ignored.
        VkDescriptorImageInfo sampler_info{};
        sampler_info.sampler = _sampler;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = Binding_Bindless::Samplers;
        write.dstArrayElement = _index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &sampler_info;

        vkUpdateDescriptorSets(device_handle, 1, &write, 0, nullptr);
    }

    // ---------- Get_registered_count ----------
    uint32_t Bindless_Registry::Get_registered_count() const
    {
        return next_free_index;
    }

    // ---------- Get_max_samplers ----------
    uint32_t Bindless_Registry::Get_max_samplers() const
    {
        return max_samplers;
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
        // Two arrays in the same set, both read only by the fragment stage
        // (nothing else samples textures yet).
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

        bindings[0].binding = Binding_Bindless::Textures;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[0].descriptorCount = max_textures;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = Binding_Bindless::Samplers;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = max_samplers;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // Per-binding flags required for bindless, the same for both arrays:
        //   PARTIALLY_BOUND   — slots that were never written are legal to
        //                       leave in the descriptor set, as long as the
        //                       shader never reads them. Essential since most
        //                       texture slots start empty, and sampler slots
        //                       past Sampler_Preset::Count are never written.
        //   UPDATE_AFTER_BIND — allows writing new slots (Register_texture,
        //                       Set_sampler) even after this set has already
        //                       been bound in a previously-recorded command
        //                       buffer.
        //   VARIABLE_DESCRIPTOR_COUNT is intentionally NOT used here — only
        //   the binding with the highest number in a set may use it, and a
        //   fixed size for both arrays is simpler and sufficient for this
        //   engine's needs.
        constexpr VkDescriptorBindingFlags bindless_flags =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

        // One entry per binding, in the same order as `bindings`.
        const std::array<VkDescriptorBindingFlags, 2> binding_flags = { bindless_flags, bindless_flags };

        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
        binding_flags_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        binding_flags_info.bindingCount = static_cast<uint32_t>(binding_flags.size());
        binding_flags_info.pBindingFlags = binding_flags.data();

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();

        // The set itself must also opt in to update-after-bind, in addition
        // to the per-binding flag above.
        layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout_info.pNext = &binding_flags_info;

        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &layout_info, nullptr, &layout),
            "Bindless_Registry: failed to create descriptor set layout");
    }

    // ---------- Create_pool ----------
    void Bindless_Registry::Create_pool()
    {
        std::array<VkDescriptorPoolSize, 2> pool_sizes{};

        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        pool_sizes[0].descriptorCount = max_textures;

        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        pool_sizes[1].descriptorCount = max_samplers;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;

        // UPDATE_AFTER_BIND_BIT on the pool is required to allocate sets
        // from a layout that uses UPDATE_AFTER_BIND bindings.
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();

        // Only one descriptor set is ever allocated from this pool — the
        // single global bindless set.
        pool_info.maxSets = 1;

        VK_CHECK(vkCreateDescriptorPool(device_handle, &pool_info, nullptr, &pool),
            "Bindless_Registry: failed to create descriptor pool");
    }

    // ---------- Create_set ----------
    void Bindless_Registry::Create_set()
    {
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &layout;

        VK_CHECK(vkAllocateDescriptorSets(device_handle, &alloc_info, &set),
            "Bindless_Registry: failed to allocate descriptor set");
    }

} // namespace Renderer