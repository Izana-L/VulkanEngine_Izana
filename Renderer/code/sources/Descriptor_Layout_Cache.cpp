#include <Descriptor_Layout_Cache.hpp>
#include <Vulkan_Utils.hpp>

#include <array>
#include <cassert>
#include <stdexcept>

namespace Renderer_System
{
    Descriptor_Layout_Cache::Descriptor_Layout_Cache(const Vulkan_Device& _device,
        VkDescriptorSetLayout _bindless_layout)
        : device_handle(_device.Get_logical_device_handle())
    {
        assert(device_handle != VK_NULL_HANDLE);
        assert(_bindless_layout != VK_NULL_HANDLE &&
            "Descriptor_Layout_Cache: el Bindless_Registry debe construirse antes");

        try
        {
            layouts[Descriptor_Set::Per_Frame] = Create_per_frame_layout();
            layouts[Descriptor_Set::Per_Pass] = Create_empty_layout();
            layouts[Descriptor_Set::Per_Material] = Create_empty_layout();
        }
        catch (...)
        {
            // Partially built: release what was created (the destructor
            // does not run for an object whose constructor threw).
            for (uint32_t set = 0; set < Descriptor_Set::Bindless; ++set)
                if (layouts[set] != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(device_handle, layouts[set], nullptr);
            throw;
        }

        layouts[Descriptor_Set::Bindless] = _bindless_layout;   // borrowed
    }

    Descriptor_Layout_Cache::~Descriptor_Layout_Cache()
    {
        // 0..2 son nuestros. El 3 es de Bindless_Registry: destruirlo aqui
        // seria un doble-destroy cuando el registro corra su destructor.
        for (uint32_t set = 0; set < Descriptor_Set::Bindless; ++set)
        {
            if (layouts[set] != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(device_handle, layouts[set], nullptr);
                layouts[set] = VK_NULL_HANDLE;
            }
        }
        layouts[Descriptor_Set::Bindless] = VK_NULL_HANDLE;
    }

    VkDescriptorSetLayout Descriptor_Layout_Cache::Get(uint32_t _set) const
    {
        assert(_set < Descriptor_Set::Count && "indice de conjunto fuera de rango");
        assert(layouts[_set] != VK_NULL_HANDLE);
        return layouts[_set];
    }

    // ---------- set 0 : por fotograma ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_per_frame_layout()
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

        // binding 0 — Frame_UBO: vista, proyeccion, sus inversas, posicion
        // de camara, tiempo. Visible en AMBAS etapas: el vertex usa las
        // matrices, el fragment usa camera_position y light_count.
        // (Hoy el layout declara solo VERTEX y mesh.frag lee este binding
        //  igualmente — ese mismatch se corrige justo aqui.)
        bindings[0].binding = Binding_Per_Frame::Frame_UBO;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT;

        // binding 1 — array de luces. STORAGE_BUFFER, no UNIFORM_BUFFER:
        // maxUniformBufferRange garantiza solo 16 KB, maxStorageBufferRange
        // al menos 128 MB, y un SSBO admite array de tamano no declarado.
        bindings[1].binding = Binding_Per_Frame::Lights;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create the set 0 layout");
        return layout;
    }

    // ---------- sets 1 y 2 : reservados ----------
    VkDescriptorSetLayout Descriptor_Layout_Cache::Create_empty_layout()
    {
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 0;
        info.pBindings = nullptr;

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(device_handle, &info, nullptr, &layout),
            "Descriptor_Layout_Cache: failed to create an empty set layout");
        return layout;
    }
}