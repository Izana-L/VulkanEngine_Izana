#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Surface.hpp>
#include "Window.hpp"

#include <vector>
#include <cstdint>

namespace Renderer_System 
{

    // Swap_chain_support_details: groups together everything we need to
    // know about what a given GPU + surface combination supports, so we
    // can pick the best available format/present mode/extent from it.
    struct Swap_chain_support_details 
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> present_modes;
    };

    // Vulkan_Swapchain: owns the VkSwapchainKHR - the set of images the GPU
    // renders into and presents to the screen, plus their VkImageView
    // wrappers (needed later to use them as render targets/framebuffers).
    //
    // Must be recreated whenever the window is resized, since the
    // swapchain images are tied to a specific surface size.
    class Vulkan_Swapchain 
    {
        VkDevice device_handle;
        VkPhysicalDevice physical_device_handle;
        VkSurfaceKHR surface_handle;

        // Stored so Recreate() can rebuild the swapchain without needing
        // these passed in again from outside.
        const Platform::Window* window;
        uint32_t preferred_image_count;
        bool prefer_mailbox;
        Queue_Family_Indices queue_family_indices;

        VkSwapchainKHR swapchain;
        std::vector<VkImage> images;          // owned by the swapchain itself, not destroyed manually
        std::vector<VkImageView> image_views;  // these we DO create and must destroy ourselves

        VkFormat image_format;
        VkExtent2D extent;
        VkPresentModeKHR selected_present_mode;
        VkQueue present_queue_handle;

    public:
        // Creates the swapchain for the given device/surface/window.
        // _preferred_image_count: how many images to request (default 3,
        // triple buffering). The driver may clamp this to what the surface
        // actually supports.
        // _prefer_mailbox: if true, tries to use VK_PRESENT_MODE_MAILBOX_KHR
        // (low latency, no tearing) and falls back to VK_PRESENT_MODE_FIFO_KHR
        // (standard VSync, always supported) if mailbox isn't available.
        Vulkan_Swapchain(
            const Vulkan_Device& _device,
            const Vulkan_Surface& _surface,
            const Platform::Window& _window,
            uint32_t _preferred_image_count = 3,
            bool _prefer_mailbox = true
        );

        ~Vulkan_Swapchain();

        Vulkan_Swapchain(const Vulkan_Swapchain&) = delete;
        Vulkan_Swapchain& operator=(const Vulkan_Swapchain&) = delete;

        Vulkan_Swapchain(Vulkan_Swapchain&& _other) noexcept;
        Vulkan_Swapchain& operator=(Vulkan_Swapchain&& _other) noexcept;

        // Destroys the current swapchain and creates a new one with the
        // window's current size. Must be called when the window is resized,
        // or when vkAcquireNextImageKHR / vkQueuePresentKHR report that the
        // swapchain is out of date.
        void Recreate();

        VkSwapchainKHR Get_handle() const;

        // The format chosen for the swapchain images - needed later by
        // Vulkan_Render_Pass to configure the color attachment correctly.
        VkFormat Get_image_format() const;

        // The actual resolution of the swapchain images - needed by
        // Vulkan_Render_Pass / Vulkan_Pipeline / Vulkan_Framebuffer for
        // viewport, scissor, and framebuffer dimensions.
        VkExtent2D Get_extent() const;

        // The image views, one per swapchain image - needed by
        // Vulkan_Framebuffer to create one framebuffer per image.
        const std::vector<VkImageView>& Get_image_views() const;

        // How many images this swapchain actually ended up with (may
        // differ from the requested _preferred_image_count if the surface
        // didn't support that many).
        uint32_t Get_image_count() const;

        // The present mode actually selected (MAILBOX or FIFO fallback) -
        // useful for logging/debug display (e.g. showing "VSync: On/Off"
        // in a settings menu based on this).
        VkPresentModeKHR Get_present_mode() const;

        

    private:
        // Destroys the swapchain and its image views, but NOT the device/
        // surface (those are owned elsewhere). Shared by the destructor,
        // move assignment, and Recreate() (which destroys the old swapchain
        // before building the new one).
        void Destroy();
        void Create_swapchain_internal();
        // Builds the swapchain itself - shared by the constructor and
        // Recreate(), since both need to do the same work, just at
        // different points in the object's lifetime.
        void Create_swapchain(const Vulkan_Device& _device,const Vulkan_Surface& _surface,const Platform::Window& _window,
                              uint32_t _preferred_image_count, bool _prefer_mailbox);
            

        // Creates one VkImageView per swapchain image - image views are
        // required to actually use the raw VkImage as a render target.
        void Create_image_views();

        // Queries the surface capabilities, formats, and present modes
        // supported by this specific GPU + surface combination.
        Swap_chain_support_details Query_swap_chain_support(VkPhysicalDevice _physical_device, VkSurfaceKHR _surface) const;

        // Picks the best color format/color space from the available
        // options. Prefers VK_FORMAT_B8G8R8A8_SRGB with
        // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, then any other sRGB format,
        // and only as a last resort the first available format (logging a
        // warning). An sRGB format is a contract with the shaders: the
        // hardware does the linear -> sRGB encode on write, so fragment
        // shaders must output LINEAR color and never apply gamma by hand.
        VkSurfaceFormatKHR Choose_surface_format(const std::vector<VkSurfaceFormatKHR>& _available_formats) const;

        // Picks the present mode: MAILBOX if available and preferred,
        // otherwise FIFO (guaranteed to always be supported by the spec).
        VkPresentModeKHR Choose_present_mode(const std::vector<VkPresentModeKHR>& _available_modes, bool _prefer_mailbox) const;

        // Determines the actual pixel resolution of the swapchain images,
        // based on the window's current framebuffer size, clamped to what
        // the surface capabilities allow (min/max extent).
        VkExtent2D Choose_extent(const VkSurfaceCapabilitiesKHR& _capabilities, const Platform::Window& _window) const;

       
    };

}