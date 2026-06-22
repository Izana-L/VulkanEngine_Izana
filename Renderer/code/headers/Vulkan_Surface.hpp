#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Instance.hpp>
#include <Window.hpp>

namespace Renderer {

    // Vulkan_surface: owns the VkSurfaceKHR, which represents the actual
    // platform-specific window surface Vulkan can render into and present to.
    //
    // This is the bridge between Platform (which only knows about the OS
    // window via GLFW) and Vulkan (which needs a VkSurfaceKHR to know
    // "where" to draw). Platform::Window deliberately knows nothing about
    // Vulkan - this class is where that connection is made.
    class Vulkan_Surface {
    public:
        // Creates a VkSurfaceKHR for the given window, using the given
        // Vulkan instance. The window must stay alive for at least as long
        // as this Vulkan_surface (the surface references the native window
        // handle internally via GLFW).
        Vulkan_Surface(const Vulkan_Instance& _instance, const Platform::Window& _window);

        ~Vulkan_Surface();

        // Surfaces should not be copied (would duplicate ownership of the
        // same VkSurfaceKHR handle, causing a double-destruction).
        Vulkan_Surface(const Vulkan_Surface&) = delete;
        Vulkan_Surface& operator=(const Vulkan_Surface&) = delete;

        Vulkan_Surface(Vulkan_Surface&& _other) noexcept;
        Vulkan_Surface& operator=(Vulkan_Surface&& _other) noexcept;

        // Raw handle, needed by Vulkan_device (to check which queue family
        // supports presenting to this surface) and Vulkan_swapchain
        // (to create the swapchain itself).
        VkSurfaceKHR Get_handle() const;

    private:
        // Destroys the VkSurfaceKHR. Shared by the destructor and move
        // assignment, same pattern as Vulkan_instance::Destroy().
        void Destroy();

        // The instance this surface was created from - needed to destroy
        // the surface correctly later (vkDestroySurfaceKHR requires the
        // VkInstance it belongs to). Stored as a raw handle, not a
        // reference, since Vulkan_surface doesn't own the instance's
        // lifetime, it just needs the handle for cleanup.
        VkInstance instance_handle;

        VkSurfaceKHR surface;
    };

}