#pragma once

#include <vulkan/vulkan.h>

#include <Vulkan_Device.hpp>
#include <Vulkan_Handles.hpp>
#include <Vulkan_Surface.hpp>

#include <vector>
#include <cstdint>
#include <optional>

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
    //
    // The swapchain does not know the window: the size it is built for is
    // passed in (the framebuffer size in pixels, read from the window by the
    // Renderer). It therefore has no way to wait for window events, and
    // never blocks: a surface without area (a minimized window) is reported
    // to the caller instead of waited for.
    //
    // The swapchain and the image views are owned by RAII members: a
    // constructor that fails halfway releases what it created.
    class Vulkan_Swapchain
    {
        VkDevice device_handle;
        VkPhysicalDevice physical_device_handle;
        VkSurfaceKHR surface_handle;

        // Stored so Recreate() can rebuild the swapchain without needing
        // these passed in again from outside.
        uint32_t preferred_image_count;
        bool prefer_mailbox;
        Queue_Family_Indices queue_family_indices;

        // The color format and color space chosen when the swapchain was
        // first created. A recreation asks for exactly this pair (see
        // Recreate), so what was built against the format stays valid.
        VkFormat image_format;
        VkColorSpaceKHR image_color_space;
        VkExtent2D extent;
        VkPresentModeKHR selected_present_mode;

        // True when the swapchain held in `swapchain` was passed as the
        // oldSwapchain of a creation that failed. Vulkan retires the old
        // swapchain whether the creation succeeds or not, and a retired
        // swapchain cannot be the oldSwapchain of another creation: the
        // next Recreate destroys it first.
        bool retired;

        // Declared in creation order: the views are destroyed first, then
        // the swapchain that owns the images they look at.
        Unique_Swapchain swapchain;
        std::vector<VkImage> images;                 // owned by the swapchain itself, not destroyed manually
        std::vector<Unique_Image_View> image_views;  // created by this class, destroyed by it (before the swapchain)

    public:
        // Creates the swapchain for the given device/surface.
        // _desired_extent: the size to build it for, in framebuffer pixels;
        // used only when the surface does not dictate its own size. Throws
        // if the surface currently has no area, or offers no sRGB format.
        // _preferred_image_count: how many images to request (default 3,
        // triple buffering). The driver may clamp this to what the surface
        // actually supports.
        // _prefer_mailbox: if true, tries to use VK_PRESENT_MODE_MAILBOX_KHR
        // (low latency, no tearing) and falls back to VK_PRESENT_MODE_FIFO_KHR
        // (standard VSync, always supported) if mailbox isn't available.
        Vulkan_Swapchain(
            const Vulkan_Device& _device,
            const Vulkan_Surface& _surface,
            VkExtent2D _desired_extent,
            uint32_t _preferred_image_count = 3,
            bool _prefer_mailbox = true
        );

        ~Vulkan_Swapchain() = default;

        Vulkan_Swapchain(const Vulkan_Swapchain&) = delete;
        Vulkan_Swapchain& operator=(const Vulkan_Swapchain&) = delete;

        Vulkan_Swapchain(Vulkan_Swapchain&& _other) noexcept = default;

        // Written by hand for the release order only: the image views of
        // the current swapchain go before the swapchain does.
        Vulkan_Swapchain& operator=(Vulkan_Swapchain&& _other) noexcept;

        // Everything Recreate needs from the surface, checked without
        // touching anything, so a caller can ask before it tears down what
        // depends on the current swapchain (the Renderer asks before it
        // waits for the device to go idle).
        //
        // Returns true when a swapchain can be built now for
        // _desired_extent: the surface has area (on some platforms, Windows,
        // it reports a 0x0 current extent while the window is minimized)
        // and still offers the format and color space of the first
        // swapchain, which every swapchain after it keeps (see Recreate).
        // Returns false when the surface has no area: a state that passes.
        // Throws std::runtime_error when the surface does not offer that
        // format and color space: it will not until the surface changes
        // again (the window moved back to another monitor).
        bool Can_recreate(VkExtent2D _desired_extent) const;

        // Replaces the swapchain with a new one for _desired_extent (the
        // framebuffer size in pixels), passing the current one as
        // oldSwapchain, and destroys the old swapchain and its image views.
        // Returns false when the surface has no area, and throws when it no
        // longer offers the format of the first swapchain (both are
        // Can_recreate); neither touches anything, the current swapchain
        // stays as it is.
        //
        // Never waits: not for the device, not for window events. The
        // caller guarantees that no pending work uses the current images
        // (Renderer::Recreate_swapchain waits for the device to go idle and
        // retires the presentation objects first).
        //
        // The new swapchain has exactly the format and color space of the
        // first one. The render pass, the pipelines and the framebuffers
        // are built against that format and cannot draw into another, and
        // the surface is free to list other formats later (the window
        // moved to another monitor).
        //
        // Otherwise not a strong guarantee: passing oldSwapchain retires it
        // even if the creation fails. In that case an exception is thrown,
        // the object keeps the retired swapchain, and the next Recreate
        // call destroys it and creates the new one from scratch.
        bool Recreate(VkExtent2D _desired_extent);

        VkSwapchainKHR Get_handle() const;

        // The format chosen for the swapchain images - needed later by
        // Vulkan_Render_Pass to configure the color attachment correctly.
        // It never changes: Recreate keeps it.
        VkFormat Get_image_format() const;

        // The actual resolution of the swapchain images - needed by
        // Vulkan_Render_Pass / Vulkan_Pipeline / Vulkan_Framebuffer for
        // viewport, scissor, and framebuffer dimensions. After a failed
        // Recreate it stays the extent of the last swapchain that was
        // successfully created.
        VkExtent2D Get_extent() const;

        // The image view of swapchain image _index - needed by
        // Vulkan_Framebuffer to create one framebuffer per image.
        VkImageView Get_image_view(uint32_t _index) const;

        // How many images this swapchain actually ended up with (may
        // differ from the requested _preferred_image_count if the surface
        // didn't support that many).
        uint32_t Get_image_count() const;

        // The present mode actually selected (MAILBOX or FIFO fallback) -
        // useful for logging/debug display (e.g. showing "VSync: On/Off"
        // in a settings menu based on this).
        VkPresentModeKHR Get_present_mode() const;

    private:
        // The constructor that does the work. _old_swapchain is passed to
        // the creation as oldSwapchain (VK_NULL_HANDLE for the first
        // swapchain). _required_format: the exact format and color space
        // the swapchain must have, or none to choose (the first swapchain).
        // The public constructor and Recreate() both use it.
        Vulkan_Swapchain(
            VkDevice _device,
            VkPhysicalDevice _physical_device,
            VkSurfaceKHR _surface,
            const Queue_Family_Indices& _queue_family_indices,
            uint32_t _preferred_image_count,
            bool _prefer_mailbox,
            VkExtent2D _desired_extent,
            VkSwapchainKHR _old_swapchain,
            std::optional<VkSurfaceFormatKHR> _required_format
        );

        // Builds the swapchain itself, with _old_swapchain as its
        // oldSwapchain, and fills the images and the format, color space,
        // extent and present mode members.
        void Create_swapchain(VkExtent2D _desired_extent, VkSwapchainKHR _old_swapchain,
                              std::optional<VkSurfaceFormatKHR> _required_format);

        // Creates one VkImageView per swapchain image - image views are
        // required to actually use the raw VkImage as a render target.
        void Create_image_views();

        // Queries the surface capabilities, formats, and present modes
        // supported by this specific GPU + surface combination.
        Swap_chain_support_details Query_swap_chain_support(VkPhysicalDevice _physical_device, VkSurfaceKHR _surface) const;

        // Picks the best color format/color space from the available
        // options. Prefers VK_FORMAT_B8G8R8A8_SRGB with
        // VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, then any other sRGB format
        // with that color space. An sRGB format is a contract with the
        // shaders: the hardware does the linear -> sRGB encode on write, so
        // fragment shaders must output LINEAR color and never apply gamma
        // by hand. When the surface offers none, throws std::runtime_error
        // instead of falling back to a format that would show wrong colors
        // without any error; Vulkan_Device only selects a GPU whose
        // surface offers one.
        //
        // _required_format: when set, nothing is chosen: the exact pair is
        // returned if the surface offers it, and std::runtime_error is
        // thrown if it does not.
        VkSurfaceFormatKHR Choose_surface_format(const std::vector<VkSurfaceFormatKHR>& _available_formats,
                                                 std::optional<VkSurfaceFormatKHR> _required_format) const;

        // Picks the present mode: MAILBOX if available and preferred,
        // otherwise FIFO (guaranteed to always be supported by the spec).
        VkPresentModeKHR Choose_present_mode(const std::vector<VkPresentModeKHR>& _available_modes, bool _prefer_mailbox) const;

        // Determines the actual pixel resolution of the swapchain images:
        // the current extent of the surface when it defines one, otherwise
        // _desired_extent clamped to what the surface capabilities allow
        // (min/max extent). A zero extent means the surface has no area.
        VkExtent2D Choose_extent(const VkSurfaceCapabilitiesKHR& _capabilities, VkExtent2D _desired_extent) const;
    };

}
