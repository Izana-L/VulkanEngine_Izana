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

    // Everything Vulkan_Device needs to know about a candidate GPU before
    // deciding whether to use it and what to enable on it. Filled by
    // Query_device_support(), read by Is_device_suitable() and by the
    // constructor when building VkDeviceCreateInfo, so the two can never
    // disagree about what was checked.
    struct Device_Support
    {
        uint32_t api_version = 0;

        // Descriptor indexing features required for bindless textures:
        //   runtimeDescriptorArray, descriptorBindingPartiallyBound,
        //   shaderSampledImageArrayNonUniformIndexing,
        //   descriptorBindingSampledImageUpdateAfterBind.
        // All four are REQUIRED: a device without them is not selected.
        bool bindless = false;

        bool sampler_anisotropy = false;
        float max_sampler_anisotropy = 1.0f;

        // Every format in Vulkan_Vertex_Layout::OPTIONAL_VERTEX_FORMATS
        // can be read from a vertex buffer. REQUIRED: the mesh pipeline
        // cannot be created without it.
        bool vertex_formats = false;
        // Device half of swapchain maintenance1: which extension name the
        // driver exposes (KHR preferred, EXT accepted), and whether the
        // feature bit behind it is supported.
        const char* swapchain_maintenance1_extension = nullptr;
        bool        swapchain_maintenance1_feature = false;

        bool swapchain_extension = false;
        bool surface_adequate = false;
        Queue_Family_Indices queue_families;
    };

    // Vulkan_Device: selects a suitable physical GPU and creates the
    // logical device (the actual "connection" to that GPU) along with the
    // queues used to submit graphics and presentation work.
    //
    // This is the second-most foundational Vulkan_* class after
    // Vulkan_Instance - almost everything else (swapchain, pipeline,
    // command pool...) needs the logical device handle to be created.
    //
    // Optional functionality is negotiated, never assumed:
    //   - VK_KHR_swapchain_maintenance1 is enabled only when the instance
    //     enabled its surface half, the device exposes the extension AND
    //     the feature bit is supported. Is_swapchain_maintenance1_enabled()
    //     tells the Renderer whether present fences may be used.
    //   - samplerAnisotropy is enabled when supported;
    //     Is_sampler_anisotropy_enabled() tells Sampler_Cache whether it
    //     may set anisotropyEnable.
    class Vulkan_Device
    {
        VkPhysicalDevice     physical_device;
        VkDevice             logical_device;
        VkQueue              graphics_queue;
        VkQueue              present_queue;
        Queue_Family_Indices queue_family_indices;
        std::string          device_name;

        bool  swapchain_maintenance1_enabled;
        bool  sampler_anisotropy_enabled;
        float max_sampler_anisotropy;

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

        // True when VK_KHR_swapchain_maintenance1 (or its EXT predecessor)
        // is enabled on this device, together with its instance half.
        bool Is_swapchain_maintenance1_enabled() const;

        // Always true for a constructed device: descriptor indexing is a
        // selection requirement. Kept so callers can state the dependency.
        bool Is_bindless_supported() const;

        // True when the samplerAnisotropy feature was enabled.
        bool  Is_sampler_anisotropy_enabled() const;
        float Get_max_sampler_anisotropy() const;

        VkFormat  Find_supported_depth_format()   const;

    private:

        void Destroy();

        std::vector<VkPhysicalDevice> Enumerate_physical_devices(VkInstance _instance) const;

        // Queries everything Is_device_suitable() and the constructor need.
        Device_Support Query_device_support(VkPhysicalDevice _device, VkSurfaceKHR _surface, const Vulkan_Instance& _instance) const;

        bool             Is_device_suitable(const Device_Support& _support) const;
        Queue_Family_Indices Find_queue_families(VkPhysicalDevice _device, VkSurfaceKHR _surface) const;
        uint32_t         Rate_device_suitability(VkPhysicalDevice _device, const Device_Support& _support) const;
        void             Log_selected_device(VkPhysicalDevice _device);
        VkFormat         Find_supported_format(const std::vector<VkFormat>& _candidates,
            VkImageTiling _tiling,
            VkFormatFeatureFlags _features) const;
    };

} // namespace Renderer
