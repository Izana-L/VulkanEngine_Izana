#include <Material_Table.hpp>
#include <Gpu_Layouts.hpp>

#include <cassert>
#include <stdexcept>
#include <string>

namespace Renderer_System
{

    Material_Table::Material_Table(VmaAllocator _allocator, uint32_t _capacity)
        : allocator(_allocator),
        capacity(_capacity)
    {
        // Host-visible and coherent, persistently mapped: Register writes
        // each new slot in place.
        buffer = Vulkan_Buffer_Utils::Create_buffer(allocator, Get_buffer_size(),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu, true);

        // Reserved up front: Register then never reallocates, so its
        // push_back cannot throw after the capacity check.
        materials.reserve(capacity);
    }

    Material_Table::~Material_Table()
    {
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, buffer);
    }

    VkDeviceSize Material_Table::Get_buffer_size() const
    {
        return sizeof(Material_GPU) * static_cast<VkDeviceSize>(capacity);
    }

    uint32_t Material_Table::Register(const Material_Desc& _desc, const Bindless_Registry& _bindless)
    {
        assert(buffer.mapped_ptr != nullptr && "Material_Table::Register: the buffer is not mapped");

        if (!_bindless.Is_texture_registered(_desc.albedo_texture_index))
            throw std::invalid_argument("Register_material: albedo_texture_index " + std::to_string(_desc.albedo_texture_index)
                + " is not a registered bindless texture slot");

        const uint32_t sampler_index = static_cast<uint32_t>(_desc.sampler);

        if (sampler_index >= static_cast<uint32_t>(CoreTypes::Sampler_Preset::Count))
            throw std::invalid_argument("Register_material: sampler " + std::to_string(sampler_index)
                + " is not a Sampler_Preset value");

        const uint32_t alpha_mode = static_cast<uint32_t>(_desc.alpha_mode);

        if (alpha_mode >= static_cast<uint32_t>(CoreTypes::Alpha_Mode::Count))
            throw std::invalid_argument("Register_material: alpha_mode " + std::to_string(alpha_mode)
                + " is not an Alpha_Mode value");

        // Deduplication by value. Linear: runs at registration time only,
        // over at most `capacity` entries.
        for (uint32_t slot = 0; slot < Get_count(); ++slot)
        {
            if (materials[slot] == _desc)
                return slot;
        }

        if (Get_count() >= capacity)
            throw std::runtime_error("Register_material: the material table is full (" + std::to_string(capacity)
                + " slots): raise MAX_MATERIALS in Renderer_Limits.hpp");

        const uint32_t slot = Get_count();
        materials.push_back(_desc);

        // Written in place: this slot was never referenced by a submitted
        // command, and host-coherent writes are visible to the next submit
        // without a flush or a barrier.
        Material_GPU& gpu_material = static_cast<Material_GPU*>(buffer.mapped_ptr)[slot];
        gpu_material.base_color = _desc.base_color;
        gpu_material.albedo_texture_index = _desc.albedo_texture_index;
        gpu_material.albedo_sampler_index = sampler_index;
        gpu_material.alpha_mode = alpha_mode;
        gpu_material.alpha_cutoff = _desc.Normalized_cutoff();

        return slot;
    }

} // namespace Renderer_System
