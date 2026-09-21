#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Instance.hpp>
#include <Vulkan_Surface.hpp>

#include <vector>
#include <optional>
#include <string>

namespace Renderer_System {

    // Queue_Family_Indices: holds the indices of the queue families this
    // device will use. A GPU exposes several queue "families", each
    // specialized in a type of work (graphics, compute, transfer,
    // presentation...). We need to find which family indices support what
    // we need before we can create the logical device.
    //
    // std::optional is used because a family might simply not exist on a
    // given GPU (e.g. some GPUs don't have a dedicated present-capable
    // family at all) - this lets us distinguish "not found yet" from "found
    // at index 0", which a plain uint32_t with a sentinel value wouldn't.
    struct Queue_Family_Indices 
    {
        std::optional<uint32_t> graphics_family;
        std::optional<uint32_t> present_family;

        bool Is_complete() const 
        {
            return graphics_family.has_value() && present_family.has_value();
        }
    };

    // Vulkan_Device: selects a suitable physical GPU and creates the
    // logical device (the actual "connection" to that GPU) along with the
    // queues used to submit graphics and presentation work.
    //
    // This is the second-most foundational Vulkan_* class after
    // Vulkan_Instance - almost everything else (swapchain, pipeline,
    // command pool...) needs the logical device handle to be created.
    class Vulkan_Device
    {
        VkPhysicalDevice     physical_device;
        VkDevice             logical_device;
        VkQueue              graphics_queue;
        VkQueue              present_queue;
        Queue_Family_Indices queue_family_indices;
        mutable std::string  device_name;

        bool swapchain_maintenance1_enabled;

        // True when the device was created with descriptor indexing features
        // required for bindless textures:
        //   - runtimeDescriptorArray
        //   - descriptorBindingPartiallyBound
        //   - shaderSampledImageArrayNonUniformIndexing
        //   - descriptorBindingSampledImageUpdateAfterBind
        // Checked by Bindless_Registry before creating the global descriptor array.
        bool bindless_supported;

    public:

        Vulkan_Device(const Vulkan_Instance& _instance, const Vulkan_Surface& _surface);
        ~Vulkan_Device();

        Vulkan_Device(const Vulkan_Device&) = delete;
        Vulkan_Device& operator=(const Vulkan_Device&) = delete;
        Vulkan_Device(Vulkan_Device&& _other) noexcept;
        Vulkan_Device& operator=(Vulkan_Device&& _other) noexcept;

        // =========================================================
        // Getters
        // =========================================================

        VkPhysicalDevice             Get_physical_device_handle()  const;
        VkDevice                     Get_logical_device_handle()    const;
        VkQueue                      Get_graphics_queue()           const;
        VkQueue                      Get_present_queue()            const;
        const Queue_Family_Indices& Get_queue_family_indices()     const;
        const std::string& Get_device_name()              const;

        bool Is_swapchain_maintenance1_enabled() const;

        // Returns true if all four bindless descriptor indexing features
        // were successfully enabled at device creation time.
        // If false, Bindless_Registry must not be used.
        bool Is_bindless_supported() const;

        VkFormat  Find_supported_depth_format()   const;
        bool      Is_swapchain_maintenance1_supported() const;


    private:

        void Destroy();

        std::vector<VkPhysicalDevice> Enumerate_physical_devices(VkInstance _instance) const;

        bool             Is_device_suitable(VkPhysicalDevice _device, VkSurfaceKHR _surface) const;
        bool             Check_device_extension_support(VkPhysicalDevice _device) const;
        Queue_Family_Indices Find_queue_families(VkPhysicalDevice _device, VkSurfaceKHR _surface) const;
        uint32_t         Rate_device_suitability(VkPhysicalDevice _device, VkSurfaceKHR _surface) const;
        std::vector<const char*> Get_required_device_extensions() const;
        void             Log_selected_device(VkPhysicalDevice _device) const;
        VkFormat         Find_supported_format(const std::vector<VkFormat>& _candidates,
            VkImageTiling _tiling,
            VkFormatFeatureFlags _features) const;

        // Returns true if all required bindless features are supported
        // by the given physical device.
        bool Check_bindless_support(VkPhysicalDevice _device) const;
    };

} // namespace Renderer