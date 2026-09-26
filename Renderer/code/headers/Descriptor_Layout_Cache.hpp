#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Descriptor_Sets.hpp>

#include <array>

namespace Renderer_System
{
    // Set 1 holds the resources written by compute passes
   // (Binding_Per_Pass), visible to the compute stage only.
   //
   // Set 2 is created with bindingCount = 0. An empty layout is legal,
   // reserves no descriptors and costs no memory: its only purpose is to
   // occupy the slot so that set 3 is ALWAYS set 3.
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
        VkDescriptorSetLayout Create_per_pass_layout();
        VkDescriptorSetLayout Create_empty_layout();
    };
}