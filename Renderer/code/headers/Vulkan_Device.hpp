#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Instance.hpp>
#include <Vulkan_Surface.hpp>

#include <vector>
#include <optional>
#include <string>

namespace Renderer {

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
    struct Queue_Family_Indices {
        std::optional<uint32_t> graphics_family;
        std::optional<uint32_t> present_family;

        // Returns true only if both required families were found
        bool Is_complete() const {
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

        VkPhysicalDevice physical_device;
        VkDevice logical_device;

        VkQueue graphics_queue;
        VkQueue present_queue;

        Queue_Family_Indices queue_family_indices;
        mutable std::string device_name;
        bool swapchain_maintenance1_enabled;

    public:
        // Picks the best available physical device and creates the logical
        // device + queues. Throws if no suitable GPU is found.
        Vulkan_Device(const Vulkan_Instance& _instance, const Vulkan_Surface& _surface);

        ~Vulkan_Device();

        Vulkan_Device(const Vulkan_Device&) = delete;
        Vulkan_Device& operator=(const Vulkan_Device&) = delete;

        Vulkan_Device(Vulkan_Device&& _other) noexcept;
        Vulkan_Device& operator=(Vulkan_Device&& _other) noexcept;

        // The chosen physical GPU handle. Needed by Vulkan_Swapchain to
        // query surface capabilities/formats supported by this specific GPU.
        VkPhysicalDevice Get_physical_device_handle() const;

        // The logical device handle. Needed by almost every other Vulkan_*
        // class to create their own resources (swapchain, pipeline, buffers...).
        VkDevice Get_logical_device_handle() const;

        // The queue used to submit graphics/draw commands
        VkQueue Get_graphics_queue() const;

        // The queue used to submit presentation commands (showing a
        // rendered image on screen). May be the same underlying queue as
        // the graphics queue on some GPUs, or a different one on others.
        VkQueue Get_present_queue() const;

        // The queue family indices found for this device - needed when
        // creating resources that must declare which queue families will
        // access them (e.g. swapchain images, command pools).
        const Queue_Family_Indices& Get_queue_family_indices() const;

        // Human-readable name of the selected GPU (e.g. "NVIDIA GeForce
        // RTX 3070") - useful for logging/debug display.
        const std::string& Get_device_name() const;

        bool Is_swapchain_maintenance1_enabled() const;

        // Finds the best supported depth format for this GPU, trying
        // candidates in order of preference. Throws if none of the
        // candidates are supported, which would be extremely unusual
        // (every Vulkan-capable GPU supports at least one depth format).
        VkFormat Find_supported_depth_format() const;

        // Returns true if VK_KHR_swapchain_maintenance1 is supported
        // on this physical device. Used by Vulkan_Swapchain to decide
        // whether to enable advanced presentation fence support.
        bool Is_swapchain_maintenance1_supported() const;

        // Finds a memory type index on this GPU that satisfies both:
        // - _type_filter: a bitmask of acceptable memory type indices,
        //   typically obtained from vkGetImageMemoryRequirements/
        //   vkGetBufferMemoryRequirements (memoryTypeBits field).
        // - _properties: required memory properties (e.g. DEVICE_LOCAL
        //   for GPU-only fast memory, or HOST_VISIBLE for memory the CPU
        //   can write to directly).
        // Used whenever allocating GPU memory for images or buffers -
        // images/buffers don't come with memory attached automatically,
        // this is how you find which memory type to request.
        uint32_t Find_memory_type(uint32_t _type_filter, VkMemoryPropertyFlags _properties) const;

        
    private:
        // Destroys the logical device. Shared by destructor and move
        // assignment. Note: the physical device is NOT destroyed here -
        // it's not something we create, just a handle to existing hardware.
        void Destroy();

        // Returns every GPU available on this system as raw VkPhysicalDevice
        // handles, without judging which one is "best" yet.
        std::vector<VkPhysicalDevice> Enumerate_physical_devices(VkInstance _instance) const;

        // Checks whether a given GPU meets our minimum requirements:
        // supports the extensions we need, and has the queue families we need.
        bool Is_device_suitable(VkPhysicalDevice _device, VkSurfaceKHR _surface) const;

        // Checks if a GPU supports all the device extensions we require
        // (at minimum, VK_KHR_swapchain to be able to present to a window).
        bool Check_device_extension_support(VkPhysicalDevice _device) const;

        // Finds which queue family indices on this GPU support graphics
        // and presentation to the given surface.
        Queue_Family_Indices Find_queue_families(VkPhysicalDevice _device, VkSurfaceKHR _surface) const;

        // Assigns a score to a GPU so we can pick the best one among all
        // suitable candidates. Higher is better; 0 means unsuitable.
        // Favors discrete GPUs over integrated ones, and considers available
        // memory and texture size limits.
        uint32_t Rate_device_suitability(VkPhysicalDevice _device, VkSurfaceKHR _surface) const;

        // Returns the list of device extensions required - currently just
        // VK_KHR_swapchain, but kept as a function in case more become
        // necessary later (e.g. for advanced rendering features).
        std::vector<const char*> Get_required_device_extensions() const;

        // Prints the chosen GPU's name and key properties to the console.
        void Log_selected_device(VkPhysicalDevice _device) const;

        // Checks each format in _candidates, in order, and returns the
        // first one that supports the given tiling and features on this
        // GPU. Used by Find_supported_depth_format, but kept generic in
        // case other format queries are needed later (e.g. for textures).
        VkFormat Find_supported_format(const std::vector<VkFormat>& _candidates,
                                       VkImageTiling _tiling,
                                       VkFormatFeatureFlags _features) const;

        
        
    };

}