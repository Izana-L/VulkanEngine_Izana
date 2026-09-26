#include <Bindless_Registry.hpp>
#include <Descriptor_Sets.hpp>
#include <Shader_Stages.hpp>
#include <Vulkan_Utils.hpp>
#include <Sampler_Preset.hpp>
#include <RenderPacket.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cassert>

namespace Renderer_System
{

    namespace
    {
        // Descriptors outside the bindless set that also count against
        // maxPerStageUpdateAfterBindResources. That limit counts sampled
        // images, storage images, buffers and input attachments of every
        // set in the pipeline layout, plus the color attachments in the
        // fragment stage; SAMPLER descriptors do not count against it. It
        // applies to each stage of Bindless_Reader_Stages separately, so
        // the margin covers the most loaded one:
        //   fragment - set 0 (frame UBO, light SSBO) and the color
        //              attachments;
        //   compute  - set 0 and the descriptors of the pass's own set 1
        //              (e.g. the storage image it writes and the textures
        //              it reads).
        // A small margin rather than an exact count, so a new per-frame or
        // per-pass binding does not silently push a stage over the limit.
        // The per-stage limits are the same numbers for every stage, so
        // Fit_to_device_limits needs no distinction between stages.
        constexpr uint32_t OTHER_STAGE_RESOURCES_MARGIN = 16;

        // Sampled images (SAMPLED_IMAGE, COMBINED_IMAGE_SAMPLER,
        // UNIFORM_TEXEL_BUFFER) that the other sets of a pipeline layout
        // may declare, e.g. an input texture that a compute pass reads
        // through its set 1. maxPerStageDescriptorUpdateAfterBindSampledImages
        // counts them per stage together with the bindless texture array,
        // and maxDescriptorSetUpdateAfterBindSampledImages over the whole
        // pipeline layout, despite its name. Set 0 declares none and sets 1
        // and 2 are empty today; a set that declares more than this must
        // raise the margin.
        constexpr uint32_t OTHER_SETS_SAMPLED_IMAGES_MARGIN = 16;

        // The same for samplers (SAMPLER, COMBINED_IMAGE_SAMPLER), which the
        // sampler limits count together with the bindless sampler array.
        constexpr uint32_t OTHER_SETS_SAMPLERS_MARGIN = 16;

        // Fewest texture slots the engine accepts; a device that cannot
        // offer them is treated as unsupported. The default textures take
        // the first slots, so the minimum must leave room beyond them.
        constexpr uint32_t MIN_BINDLESS_TEXTURES = 64;

        static_assert(MIN_BINDLESS_TEXTURES > CoreTypes::Default_Texture::Count,"MIN_BINDLESS_TEXTURES must leave room for real textures after the default ones");

        struct Array_Sizes
        {
            uint32_t textures = 0;
            uint32_t samplers = 0;
        };

        // _limit minus _margin, or 0 when the margin does not fit.
        constexpr uint32_t Reserve_margin(uint32_t _limit, uint32_t _margin)
        {
            return _limit > _margin ? _limit - _margin : 0;
        }

        // Clamps the requested sizes to the device limits:
        //   - each array is capped by its own per-stage and per-layout
        //     limits, minus what the other sets may declare in the same
        //     category;
        //   - the texture array alone is capped by the resources-per-stage
        //     budget, which counts sampled images but not samplers;
        //   - both arrays together must fit in the descriptors of all
        //     update-after-bind pools, which count every descriptor type.
        //     The bindless pool is the only pool created with
        //     UPDATE_AFTER_BIND, so the whole limit is available to it.
        //     When the sum does not fit, only the texture array shrinks:
        //     the sampler array is tiny and its slots are fixed by the
        //     presets.
        Array_Sizes Fit_to_device_limits(const Array_Sizes& _desired, const Bindless_Limits& _limits)
        {
            Array_Sizes sizes;

            sizes.textures = std::min({ _desired.textures,
                                        Reserve_margin(_limits.max_per_stage_sampled_images, OTHER_SETS_SAMPLED_IMAGES_MARGIN),
                                        Reserve_margin(_limits.max_per_set_sampled_images, OTHER_SETS_SAMPLED_IMAGES_MARGIN),
                                        Reserve_margin(_limits.max_per_stage_resources, OTHER_STAGE_RESOURCES_MARGIN) });

            sizes.samplers = std::min({ _desired.samplers,
                                        Reserve_margin(_limits.max_per_stage_samplers, OTHER_SETS_SAMPLERS_MARGIN),
                                        Reserve_margin(_limits.max_per_set_samplers, OTHER_SETS_SAMPLERS_MARGIN) });

            // Summed in 64 bits: two limits close to UINT32_MAX must not
            // wrap around and appear to fit.
            const uint64_t pool_descriptors = static_cast<uint64_t>(sizes.textures) + sizes.samplers;

            if (pool_descriptors > _limits.max_descriptors_in_all_pools)
            {
                sizes.textures = _limits.max_descriptors_in_all_pools > sizes.samplers
                               ? _limits.max_descriptors_in_all_pools - sizes.samplers : 0;
            }

            return sizes;
        }
    }

    // ---------- Constructor ----------
    Bindless_Registry::Bindless_Registry(const Vulkan_Device& _device, uint32_t _desired_textures, uint32_t _desired_samplers)
        : device_handle(_device.Get_logical_device_handle()),
        max_textures(_desired_textures),
        max_samplers(_desired_samplers),
        layout(VK_NULL_HANDLE),
        pool(VK_NULL_HANDLE),
        set(VK_NULL_HANDLE)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Bindless_Registry");

        // Enforced in every build: creating a layout with the
        // UPDATE_AFTER_BIND / UPDATE_UNUSED_WHILE_PENDING / PARTIALLY_BOUND
        // flags on a device that did not enable descriptor indexing is
        // invalid usage that no layer may be present to report.
        // Is_bindless_supported() reports the features the device was
        // actually created with. Vulkan_Device only selects devices that
        // have them, so this is a defence against a future change to that
        // policy, not an expected path.
        if (!_device.Is_bindless_supported())
        {
            throw std::runtime_error(
                "Bindless_Registry: the device was created without the descriptor indexing "
                "features bindless textures require");
        }

        if (max_textures == 0)
            throw std::invalid_argument("Bindless_Registry: _desired_textures must be greater than zero");

        if (max_samplers == 0)
            throw std::invalid_argument("Bindless_Registry: _desired_samplers must be greater than zero");

        // ── Effective sizes ────────────────────────────────────────
        // Up to here max_textures / max_samplers hold the requests; from
        // here on they hold what the device can actually offer.
        const Array_Sizes effective = Fit_to_device_limits({ max_textures, max_samplers }, _device.Get_bindless_limits());

        const bool clamped = effective.textures != max_textures || effective.samplers != max_samplers;

        std::ostream& log = clamped ? std::cerr : std::cout;
        log << "[Bindless_Registry] Textures: desired " << max_textures << ", effective " << effective.textures
            << ". Samplers: desired " << max_samplers << ", effective " << effective.samplers
            << (clamped ? ". Clamped to the device limits.\n" : ".\n");

        const uint32_t preset_count = static_cast<uint32_t>(CoreTypes::Sampler_Preset::Count);

        if (effective.samplers < preset_count) 
        {
            throw std::runtime_error("Bindless_Registry: the device allows " + std::to_string(effective.samplers) +
                                     " bindless samplers, but the " + std::to_string(preset_count) + " sampler presets need one slot each");
                
        }

        if (effective.textures < MIN_BINDLESS_TEXTURES) 
        {
            throw std::runtime_error("Bindless_Registry: the device allows " + std::to_string(effective.textures) +
                                     " bindless textures, fewer than the minimum of " + std::to_string(MIN_BINDLESS_TEXTURES));
        }

        max_textures = effective.textures;
        max_samplers = effective.samplers;

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

        uint32_t index = 0;

        // The bookkeeping is updated before the descriptor write: the only
        // step that can throw (push_back) then runs before anything changes.
        if (slot_views.size() < max_textures)
        {
            // A slot that was never used: no frame has ever read it.
            index = static_cast<uint32_t>(slot_views.size());
            slot_views.push_back(_image_view);
        }
        else if (!released_slots.empty())
        {
            // No frame in flight reads a released slot: Release_texture
            // required it when the slot was released, and no command
            // recorded since then may use a released index.
            index = released_slots.front();
            released_slots.pop_front();
            slot_views[index] = _image_view;
        }
        else
        {
            throw std::runtime_error( "Bindless_Registry: the texture array is full (" + std::to_string(max_textures) +
                                      " slots, none released): release unused slots or raise BINDLESS_DESIRED_TEXTURES, "
                                      "within the device limits logged at startup." );
        }

        Write_texture_slot(index, _image_view);

        return index;
    }

    // ---------- Update_texture ----------
    void Bindless_Registry::Update_texture(uint32_t _index, VkImageView _image_view)
    {
        if (_image_view == VK_NULL_HANDLE)
            throw std::invalid_argument("Bindless_Registry::Update_texture: null image view");

        Validate_mutable_slot(_index, "Update_texture");

        slot_views[_index] = _image_view;
        Write_texture_slot(_index, _image_view);
    }

    // ---------- Release_texture ----------
    void Bindless_Registry::Release_texture(uint32_t _index)
    {
        Validate_mutable_slot(_index, "Release_texture");

        // Queued first: push_back is the only step that can throw, so a
        // failure leaves the slot registered and unchanged.
        released_slots.push_back(_index);

        // A stale index reads the error texture instead of the released
        // view. The Renderer registers the error texture before anything
        // else, so it is always present by the time a slot can be released;
        // without it, the slot keeps its previous descriptor, which
        // PARTIALLY_BOUND tolerates as long as no shader reads the slot.
        constexpr uint32_t fallback_index = CoreTypes::Default_Texture::Error;

        if (Is_texture_registered(fallback_index))
            Write_texture_slot(_index, slot_views[fallback_index]);

        slot_views[_index] = VK_NULL_HANDLE;
    }

    // ---------- Is_texture_registered ----------
    bool Bindless_Registry::Is_texture_registered(uint32_t _index) const
    {
        return _index < slot_views.size() && slot_views[_index] != VK_NULL_HANDLE;
    }

    // ---------- Write_texture_slot ----------
    void Bindless_Registry::Write_texture_slot(uint32_t _index, VkImageView _image_view)
    {
        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = _image_view;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = Binding_Bindless::Textures;
        write.dstArrayElement = _index;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;

        // Every caller guarantees that no command buffer pending execution
        // reads this slot, which is what makes the write legal while frames
        // that use the set are still in flight: UPDATE_UNUSED_WHILE_PENDING
        // allows writing the descriptors those frames do not read, and
        // UPDATE_AFTER_BIND a command buffer that bound the set and is
        // still being recorded, whose submission then sees the new view.
        vkUpdateDescriptorSets(device_handle, 1, &write, 0, nullptr);
    }

    // ---------- Validate_mutable_slot ----------
    void Bindless_Registry::Validate_mutable_slot(uint32_t _index, const char* _caller) const
    {
        if (_index < CoreTypes::Default_Texture::Count)
        {
            throw std::invalid_argument(std::string("Bindless_Registry::") + _caller + ": slot " + std::to_string(_index) +
                                        " holds a default texture (CoreTypes::Default_Texture), which every draw item relies on");
        }

        if (!Is_texture_registered(_index))
        {
            throw std::invalid_argument(std::string("Bindless_Registry::") + _caller + ": slot " + std::to_string(_index) +
                                        " does not hold a registered texture");
        }
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
        return static_cast<uint32_t>(slot_views.size() - released_slots.size());
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
        // Two arrays in the same set, both visible to every stage in
        // Bindless_Reader_Stages (Shader_Stages.hpp). The barriers that
        // leave an image readable through this set wait on the matching
        // Bindless_Reader_Pipeline_Stages, so a stage added there is
        // covered by the layout and by the barriers at once.
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

        bindings[0].binding = Binding_Bindless::Textures;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[0].descriptorCount = max_textures;
        bindings[0].stageFlags = Bindless_Reader_Stages;

        bindings[1].binding = Binding_Bindless::Samplers;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = max_samplers;
        bindings[1].stageFlags = Bindless_Reader_Stages;

        // Per-binding flags required for bindless, the same for both arrays:
        //   PARTIALLY_BOUND   — slots that were never written are legal to
        //                       leave in the descriptor set, as long as the
        //                       shader never reads them. Essential since most
        //                       texture slots start empty, and sampler slots
        //                       past Sampler_Preset::Count are never written.
        //   UPDATE_AFTER_BIND — writes made after this set was bound in a
        //                       command buffer that is still being recorded
        //                       are legal, and its submission sees them.
        //   UPDATE_UNUSED_WHILE_PENDING
        //                     — slots that no pending command buffer reads
        //                       (dynamically, with PARTIALLY_BOUND) can be
        //                       written while frames that use this set are
        //                       still executing. Register_texture,
        //                       Update_texture, Release_texture and
        //                       Set_sampler rely on it outside startup.
        //   VARIABLE_DESCRIPTOR_COUNT is intentionally NOT used here — only
        //   the binding with the highest number in a set may use it, and a
        //   fixed size for both arrays is simpler and sufficient for this
        //   engine's needs.
        constexpr VkDescriptorBindingFlags bindless_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

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

        // Final word from the driver. The limits used to size the arrays
        // are per category; a layout can still be rejected as a whole.
        // Asking first makes the failure carry the sizes instead of a
        // bare VkResult from vkCreateDescriptorSetLayout.
        VkDescriptorSetLayoutSupport layout_support{};
        layout_support.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
        vkGetDescriptorSetLayoutSupport(device_handle, &layout_info, &layout_support);

        if (layout_support.supported != VK_TRUE) 
        {
            throw std::runtime_error( "Bindless_Registry: the device does not support a bindless layout with " +
                                      std::to_string(max_textures) + " textures and " + std::to_string(max_samplers) + " samplers");
        }

        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &layout_info, nullptr, &layout),"Bindless_Registry: failed to create descriptor set layout");
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