#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <Bindless_Registry.hpp>
#include <Material_Desc.hpp>
#include <Vulkan_Buffer_Utils.hpp>

#include <cstdint>
#include <vector>

namespace Renderer_System
{

    // Material_Table: the GPU material table (set 2, Binding_Per_Material::
    // Materials) and the registry of the materials written into it.
    //
    // Append-only: a slot, once written, never changes and is never
    // released, so a slot handed out stays valid for the lifetime of the
    // table. Objects reference a material by slot (Object_GPU::
    // material_index) and the shaders read the entry from the table.
    //
    // Registration deduplicates by value: a description equal to a
    // registered one returns its slot and writes nothing.
    //
    // One copy of the buffer serves every frame in flight. It is
    // persistently mapped and host-coherent, and a new slot is never
    // referenced by a command already submitted, so an in-place write
    // needs no barrier and no per-frame copy.
    //
    // Not copyable or movable: owns the buffer.
    class Material_Table
    {
    public:

        // Creates the buffer for _capacity materials (Material_GPU each).
        Material_Table(VmaAllocator _allocator, uint32_t _capacity);
        ~Material_Table();

        Material_Table(const Material_Table&) = delete;
        Material_Table& operator=(const Material_Table&) = delete;
        Material_Table(Material_Table&&) = delete;
        Material_Table& operator=(Material_Table&&) = delete;

        // Registers _desc and returns its slot. Meant for load time;
        // calling it between frames is also valid.
        //
        // The texture is checked here, once, instead of once per draw per
        // frame. A slot that passes stays valid provided the texture is not
        // released through Bindless_Registry::Release_texture while a
        // material references it (nothing releases textures today).
        //
        // Throws std::invalid_argument if albedo_texture_index is not a
        // registered bindless slot or sampler is not a Sampler_Preset
        // value, and std::runtime_error if the table is full.
        uint32_t Register(const Material_Desc& _desc, const Bindless_Registry& _bindless);

        // Slots in use; the next slot to be handed out.
        uint32_t Get_count() const { return static_cast<uint32_t>(materials.size()); }

        uint32_t Get_capacity() const { return capacity; }

        VkBuffer Get_buffer() const { return buffer.buffer; }

        // Size of the whole buffer, the range a descriptor covers: the
        // descriptor is written once for the capacity and entries are added
        // in place afterwards.
        VkDeviceSize Get_buffer_size() const;

    private:

        VmaAllocator                             allocator;
        uint32_t                                 capacity;
        Vulkan_Buffer_Utils::Buffer_Allocation   buffer;

        // CPU copy of every registered description; index = slot.
        std::vector<Material_Desc>               materials;
    };

} // namespace Renderer_System
