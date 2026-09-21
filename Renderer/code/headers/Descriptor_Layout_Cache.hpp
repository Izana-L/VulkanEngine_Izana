#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Descriptor_Sets.hpp>

#include <array>

namespace Renderer_System
{
    // Descriptor_Layout_Cache: duena de los VkDescriptorSetLayout de los
    // conjuntos 0, 1 y 2, y portadora (NO duena) del 3.
    //
    // El 3 lo fabrica Bindless_Registry, que necesita flags propios
    // (PARTIALLY_BOUND, UPDATE_AFTER_BIND) y su propio pool. Aqui solo se
    // guarda el handle para poder entregar los cuatro seguidos a
    // vkCreatePipelineLayout — por eso el destructor destruye 0..2 y deja
    // el 3 en paz.
    //
    // Los conjuntos 1 y 2 se crean con bindingCount = 0. Un layout vacio
    // es legal, no reserva descriptores y no cuesta memoria: su unica
    // funcion es ocupar la ranura para que el 3 este SIEMPRE en el 3.
    class Descriptor_Layout_Cache
    {
        VkDevice device_handle;
        std::array<VkDescriptorSetLayout, Descriptor_Set::Count> layouts{};

    public:

        Descriptor_Layout_Cache(const Vulkan_Device& _device,
            VkDescriptorSetLayout _bindless_layout);
        ~Descriptor_Layout_Cache();

        Descriptor_Layout_Cache(const Descriptor_Layout_Cache&) = delete;
        Descriptor_Layout_Cache& operator=(const Descriptor_Layout_Cache&) = delete;
        Descriptor_Layout_Cache(Descriptor_Layout_Cache&&) = delete;
        Descriptor_Layout_Cache& operator=(Descriptor_Layout_Cache&&) = delete;

        // Para asignar conjuntos de ese layout (vkAllocateDescriptorSets).
        VkDescriptorSetLayout Get(uint32_t _set) const;

        // Los cuatro seguidos, para vkCreatePipelineLayout.
        const VkDescriptorSetLayout* Data() const { return layouts.data(); }

    private:

        VkDescriptorSetLayout Create_per_frame_layout();
        VkDescriptorSetLayout Create_empty_layout();
    };
}