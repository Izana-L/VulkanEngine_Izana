#pragma once

#include <vulkan/vulkan.h>

#include <array>

namespace Renderer_System
{

    // Swapchain maintenance1 exists under two names: the original EXT
    // extensions and the KHR ones they were promoted to. Each name has two
    // halves, an instance extension and a device extension, and the
    // specification ties each device extension to the instance extension of
    // ITS OWN name: VK_EXT_swapchain_maintenance1 depends on
    // VK_EXT_surface_maintenance1, and VK_KHR_swapchain_maintenance1 on
    // VK_KHR_surface_maintenance1. A device extension enabled next to the
    // instance extension of the other name is invalid usage.
    //
    // The pairing is therefore declared once, here, and both sides read it:
    //   - Vulkan_Instance enables the instance half of every variant the
    //     loader reports (the loader unions the extensions of all the
    //     drivers, so the variants a given GPU supports can differ);
    //   - Vulkan_Device picks, for the selected GPU, the first variant whose
    //     instance half is enabled AND whose device half the GPU exposes.
    // Instance and device never choose independently, so they cannot end up
    // with halves of different names.
    //
    // KHR comes first: it is preferred when both are available.
    struct Swapchain_Maintenance1_Variant
    {
        const char* surface_extension;          // instance extension
        const char* swapchain_extension;        // device extension
        const char* release_images_function;    // entry point that gives back an acquired image
    };

    inline constexpr std::array<Swapchain_Maintenance1_Variant, 2> SWAPCHAIN_MAINTENANCE1_VARIANTS =
    { {
        { VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, "vkReleaseSwapchainImagesKHR" },
        { VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, "vkReleaseSwapchainImagesEXT" }
    } };

}
