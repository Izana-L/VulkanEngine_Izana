#include <Descriptor_Layout_Cache.hpp>
#include <Descriptor_Layouts.hpp>
#include <Vulkan_Utils.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <stdexcept>
#include <string>

namespace Renderer_System
{
    Descriptor_Layout_Cache::Descriptor_Layout_Cache(const Vulkan_Device& _device,
        VkDescriptorSetLayout _bindless_layout)
        : device_handle(_device.Get_logical_device_handle())
    {
        assert(device_handle != VK_NULL_HANDLE);

        // The null layout would go into the pipeline layouts as set 3.
        if (_bindless_layout == VK_NULL_HANDLE)
            throw std::invalid_argument("Descriptor_Layout_Cache: the Bindless_Registry must be built first (null bindless layout)");

        try
        {
            const VkDescriptorSetLayout per_frame = Create_layout(Descriptor_Layouts::Per_Frame,
                "Descriptor_Layout_Cache: failed to create the set 0 layout");
            compute_layouts[Descriptor_Set::Per_Frame] = per_frame;
            graphics_layouts[Descriptor_Set::Per_Frame] = per_frame;

            compute_layouts[Descriptor_Set::Per_Pass] = Create_layout(Descriptor_Layouts::Compute_Per_Pass,
                "Descriptor_Layout_Cache: failed to create the set 1 layout");
            graphics_layouts[Descriptor_Set::Per_Pass] = Create_layout(Descriptor_Layouts::Graphics_Per_Pass,
                "Descriptor_Layout_Cache: failed to create the graphics set 1 layout");

            const VkDescriptorSetLayout per_material = Create_layout(Descriptor_Layouts::Per_Material,
                "Descriptor_Layout_Cache: failed to create the set 2 layout");
            compute_layouts[Descriptor_Set::Per_Material] = per_material;
            graphics_layouts[Descriptor_Set::Per_Material] = per_material;

            Check_storage_buffer_budget();
        }
        catch (...)
        {
            // Partially built: release what was created (the destructor
            // does not run for an object whose constructor threw).
            Destroy_owned_layouts();
            throw;
        }

        compute_layouts[Descriptor_Set::Bindless] = _bindless_layout;   // borrowed
        graphics_layouts[Descriptor_Set::Bindless] = _bindless_layout;
    }

    Descriptor_Layout_Cache::~Descriptor_Layout_Cache()
    {
        Destroy_owned_layouts();

        compute_layouts[Descriptor_Set::Bindless] = VK_NULL_HANDLE;
        graphics_layouts[Descriptor_Set::Bindless] = VK_NULL_HANDLE;
    }

    void Descriptor_Layout_Cache::Destroy_owned_layouts()
    {
        // Sets 0 and 2 are shared by both arrays: destroyed once, through
        // the compute array. Set 1 is distinct in each. Set 3 is borrowed.
        const auto destroy = [&](VkDescriptorSetLayout& _layout)
            {
                if (_layout != VK_NULL_HANDLE)
                {
                    vkDestroyDescriptorSetLayout(device_handle, _layout, nullptr);
                    _layout = VK_NULL_HANDLE;
                }
            };

        destroy(compute_layouts[Descriptor_Set::Per_Frame]);
        destroy(compute_layouts[Descriptor_Set::Per_Pass]);
        destroy(compute_layouts[Descriptor_Set::Per_Material]);
        destroy(graphics_layouts[Descriptor_Set::Per_Pass]);

        graphics_layouts[Descriptor_Set::Per_Frame] = VK_NULL_HANDLE;
        graphics_layouts[Descriptor_Set::Per_Material] = VK_NULL_HANDLE;
    }

    VkDescriptorSetLayout Descriptor_Layout_Cache::Get(uint32_t _set, Pipeline_Kind _kind) const
    {
        // The set number indexes an array: checked in every build.
        if (_set >= Descriptor_Set::Count)
        {
            throw std::out_of_range("Descriptor_Layout_Cache::Get: set " + std::to_string(_set) + " is out of range (" +
                                    std::to_string(Descriptor_Set::Count) + " sets)");
        }

        const VkDescriptorSetLayout layout = Data(_kind)[_set];

        if (layout == VK_NULL_HANDLE)
            throw std::logic_error("Descriptor_Layout_Cache::Get: set " + std::to_string(_set) + " has no layout");

        return layout;
    }

    const VkDescriptorSetLayout* Descriptor_Layout_Cache::Data(Pipeline_Kind _kind) const
    {
        return (_kind == Pipeline_Kind::Graphics) ? graphics_layouts.data() : compute_layouts.data();
    }

    // ---------- layout creation ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_layout(std::span<const Vulkan_Descriptor_Utils::Layout_Binding> _bindings,
                                                                 const char* _what)
    {
        Record_bindings(_bindings);

        return Vulkan_Descriptor_Utils::Create_set_layout(device_handle, _bindings, _what);
    }

    // ---------- storage buffer budget ----------
    void Descriptor_Layout_Cache::Record_bindings(std::span<const Vulkan_Descriptor_Utils::Layout_Binding> _bindings)
    {
        for (const Vulkan_Descriptor_Utils::Layout_Binding& binding : _bindings)
        {
            if (binding.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                continue;

            if (binding.stages & VK_SHADER_STAGE_VERTEX_BIT)   storage_buffers_per_stage[0] += binding.count;
            if (binding.stages & VK_SHADER_STAGE_FRAGMENT_BIT) storage_buffers_per_stage[1] += binding.count;
            if (binding.stages & VK_SHADER_STAGE_COMPUTE_BIT)  storage_buffers_per_stage[2] += binding.count;

            storage_buffers_total += binding.count;
        }
    }

    void Descriptor_Layout_Cache::Check_storage_buffer_budget() const
    {
        // Required_Storage_Buffers is what Vulkan_Device demanded from the
        // GPU. Layouts beyond it could exceed the limits of a device that
        // was accepted, so the mismatch is a programming error reported
        // here, not a validation message on some GPUs only. The bindless
        // set holds no storage buffers.
        const uint32_t per_stage = std::max({ storage_buffers_per_stage[0], storage_buffers_per_stage[1], storage_buffers_per_stage[2] });

        if (per_stage > Required_Storage_Buffers || storage_buffers_total > Required_Storage_Buffers)
        {
            throw std::logic_error("Descriptor_Layout_Cache: the layouts use " + std::to_string(per_stage) +
                " storage buffers in one stage and " + std::to_string(storage_buffers_total) +
                " in total, more than Required_Storage_Buffers (" + std::to_string(Required_Storage_Buffers) +
                "): raise it in Descriptor_Sets.hpp");
        }
    }
}
