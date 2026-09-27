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
       // Set 2 holds the global material table (Binding_Per_Material),
    // visible to the fragment stage (material sampling) and the compute
    // stage (passes that read materials, e.g. GPU culling).
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
        VkDescriptorSetLayout Create_per_material_layout();
    };
}