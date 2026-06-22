#include "Vulkan_Swapchain.hpp"
#include "Vulkan_Utils.hpp"

#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <limits>

namespace Renderer 
{

    // ---------- Constructor ----------
    Vulkan_Swapchain::Vulkan_Swapchain(
        const Vulkan_Device& _device,
        const Vulkan_Surface& _surface,
        const Platform::Window& _window,
        uint32_t _preferred_image_count,
        bool _prefer_mailbox)

        : device_handle(_device.Get_logical_device_handle()),
        physical_device_handle(_device.Get_physical_device_handle()),
        surface_handle(_surface.Get_handle()),
        present_queue_handle(_device.Get_present_queue()),
        window(&_window),
        preferred_image_count(_preferred_image_count),
        prefer_mailbox(_prefer_mailbox),
        queue_family_indices(_device.Get_queue_family_indices()),
        swapchain(VK_NULL_HANDLE),
        image_format(VK_FORMAT_UNDEFINED),
        extent{ 0, 0 },
        selected_present_mode(VK_PRESENT_MODE_FIFO_KHR) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a swapchain");
        assert(surface_handle != VK_NULL_HANDLE && "Vulkan_Surface must be fully constructed before creating a swapchain");
        assert(_preferred_image_count >= 1 && "Preferred image count must be at least 1");
        assert(present_queue_handle != VK_NULL_HANDLE && "Vulkan_Device must have a valid present queue before creating a swapchain");

        Create_swapchain(_device, _surface, _window, _preferred_image_count, _prefer_mailbox);
        Create_image_views();
    }

    // ---------- Destructor ----------
    Vulkan_Swapchain::~Vulkan_Swapchain() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Swapchain::Destroy() {
        // Image views are something WE created explicitly, so we must
        // destroy them ourselves before destroying the swapchain.
        for (VkImageView view : image_views) {
            vkDestroyImageView(device_handle, view, nullptr);
        }
        image_views.clear();

        // The swapchain images themselves (VkImage) are owned and managed
        // by the swapchain - destroying the swapchain automatically
        // destroys them, we must NOT call vkDestroyImage on them ourselves.
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_handle, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
    }

    // ---------- Recreate ----------
    void Vulkan_Swapchain::Recreate() {
        assert(swapchain != VK_NULL_HANDLE && "Recreate() called on a moved-from or already-destroyed Vulkan_Swapchain");

        // Handle minimization: if the window is minimized, the framebuffer
        // size is 0x0, and Vulkan doesn't allow creating a swapchain with
        // zero extent. We wait here, polling events, until the window has
        // a valid size again (user restores it) before proceeding.
        int width = 0, height = 0;
        window->Get_framebuffer_size(width, height);
        while (width == 0 || height == 0) {
            window->Get_framebuffer_size(width, height);
            glfwWaitEvents();
        }

        // Wait for the device to be idle before destroying the old
        // swapchain - we must be certain the GPU isn't still using the
        // old images/views before we destroy them.
        vkDeviceWaitIdle(device_handle);

        Destroy();

        // Rebuild everything using the same parameters as the original
        // construction. Note: queue_family_indices, prefer_mailbox, etc.
        // are already cached as members, so we don't need them passed in again.
        VkSurfaceKHR temp_surface = surface_handle; // already have it cached

        Swap_chain_support_details support = Query_swap_chain_support(physical_device_handle, surface_handle);
        VkSurfaceFormatKHR surface_format = Choose_surface_format(support.formats);
        VkPresentModeKHR present_mode = Choose_present_mode(support.present_modes, prefer_mailbox);
        VkExtent2D new_extent = Choose_extent(support.capabilities, *window);

        uint32_t image_count = preferred_image_count;
        if (support.capabilities.maxImageCount > 0) {
            image_count = std::min(image_count, support.capabilities.maxImageCount);
        }
        image_count = std::max(image_count, support.capabilities.minImageCount);

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = surface_handle;
        create_info.minImageCount = image_count;
        create_info.imageFormat = surface_format.format;
        create_info.imageColorSpace = surface_format.colorSpace;
        create_info.imageExtent = new_extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        uint32_t queue_family_indices_array[] = {
            queue_family_indices.graphics_family.value(),
            queue_family_indices.present_family.value()
        };

        if (queue_family_indices.graphics_family != queue_family_indices.present_family) {
            create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            create_info.queueFamilyIndexCount = 2;
            create_info.pQueueFamilyIndices = queue_family_indices_array;
        }
        else {
            create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            create_info.queueFamilyIndexCount = 0;
            create_info.pQueueFamilyIndices = nullptr;
        }

        create_info.preTransform = support.capabilities.currentTransform;
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode = present_mode;
        create_info.clipped = VK_TRUE;
        create_info.oldSwapchain = VK_NULL_HANDLE;

        VkResult result = vkCreateSwapchainKHR(device_handle, &create_info, nullptr, &swapchain);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to recreate swapchain: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        image_format = surface_format.format;
        extent = new_extent;
        selected_present_mode = present_mode;

        uint32_t actual_image_count = 0;
        vkGetSwapchainImagesKHR(device_handle, swapchain, &actual_image_count, nullptr);
        images.resize(actual_image_count);
        vkGetSwapchainImagesKHR(device_handle, swapchain, &actual_image_count, images.data());

        Create_image_views();

        std::cout << "[Vulkan_Swapchain] Swapchain recreated: " << extent.width << "x" << extent.height
            << ", " << actual_image_count << " images.\n";
    }
    // ---------- Acquire_next_image ----------
    bool Vulkan_Swapchain::Acquire_next_image(
        VkSemaphore _image_available_semaphore,
        uint32_t& _out_image_index,
        uint64_t _timeout) {

        assert(swapchain != VK_NULL_HANDLE && "Acquire_next_image() called on a moved-from or destroyed Vulkan_Swapchain");
        assert(_image_available_semaphore != VK_NULL_HANDLE && "Acquire_next_image() called with a null semaphore");

        // vkAcquireNextImageKHR asks the swapchain which image index is
        // next available to render into. It does NOT block on the CPU
        // side waiting for the image to be ready - instead, it signals
        // _image_available_semaphore on the GPU once that image actually
        // becomes available, so the GPU itself can wait on that semaphore
        // before starting to draw into it.
        VkResult result = vkAcquireNextImageKHR(
            device_handle,
            swapchain,
            _timeout,
            _image_available_semaphore,
            VK_NULL_HANDLE, // no fence used here - synchronization is handled via the semaphore instead
            &_out_image_index
        );

        // VK_ERROR_OUT_OF_DATE_KHR means the swapchain no longer matches
        // the surface (typically after a resize) and MUST be recreated
        // before it can be used again - this is an expected, recoverable
        // condition, not a real error to throw on.
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return false;
        }

        // VK_SUBOPTIMAL_KHR means the swapchain CAN still be used this
        // frame, but it's no longer an exact match for the surface
        // (e.g. the window was resized but the old swapchain still
        // technically works). We treat it the same as out-of-date here -
        // simpler to just trigger a recreation than to render one
        // slightly-mismatched frame and recreate next time.
        if (result == VK_SUBOPTIMAL_KHR) {
            return false;
        }

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to acquire swapchain image: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        return true;
    }

    // ---------- Present ----------
    bool Vulkan_Swapchain::Present(VkSemaphore _render_finished_semaphore,uint32_t _image_index,VkFence _present_fence) 
    {
        
        assert(swapchain != VK_NULL_HANDLE && "Present() called on a moved-from or destroyed Vulkan_Swapchain");
        assert(_render_finished_semaphore != VK_NULL_HANDLE && "Present() called with a null semaphore");

        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &_render_finished_semaphore;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &_image_index;

        // If VK_KHR_swapchain_maintenance1 is available, attach the
        // present fence so we know exactly when the presentation engine
        // is done with this image's semaphore.
        VkSwapchainPresentFenceInfoEXT fence_info{};
        if (_present_fence != VK_NULL_HANDLE) {
            fence_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
            fence_info.swapchainCount = 1;
            fence_info.pFences = &_present_fence;
            present_info.pNext = &fence_info;
        }

        VkResult result = vkQueuePresentKHR(present_queue_handle, &present_info);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            return false;
        }

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to present swapchain image: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        return true;
    }
    // ---------- Move constructor ----------
    Vulkan_Swapchain::Vulkan_Swapchain(Vulkan_Swapchain&& _other) noexcept
        : device_handle(_other.device_handle),
        physical_device_handle(_other.physical_device_handle),
        surface_handle(_other.surface_handle),
        present_queue_handle(_other.present_queue_handle),
        window(_other.window),
        preferred_image_count(_other.preferred_image_count),
        prefer_mailbox(_other.prefer_mailbox),
        queue_family_indices(_other.queue_family_indices),
        swapchain(_other.swapchain),
        images(std::move(_other.images)),
        image_views(std::move(_other.image_views)),
        image_format(_other.image_format),
        extent(_other.extent),
        selected_present_mode(_other.selected_present_mode) {

        _other.swapchain = VK_NULL_HANDLE;
        _other.image_views.clear();
    }

    // ---------- Move assignment ----------
    Vulkan_Swapchain& Vulkan_Swapchain::operator=(Vulkan_Swapchain&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            physical_device_handle = _other.physical_device_handle;
            surface_handle = _other.surface_handle;
            present_queue_handle = _other.present_queue_handle;
            window = _other.window;
            preferred_image_count = _other.preferred_image_count;
            prefer_mailbox = _other.prefer_mailbox;
            queue_family_indices = _other.queue_family_indices;
            swapchain = _other.swapchain;
            images = std::move(_other.images);
            image_views = std::move(_other.image_views);
            image_format = _other.image_format;
            extent = _other.extent;
            selected_present_mode = _other.selected_present_mode;

            _other.swapchain = VK_NULL_HANDLE;
            _other.image_views.clear();
        }
        return *this;
    }

    // ---------- Get_handle ----------
    VkSwapchainKHR Vulkan_Swapchain::Get_handle() const {
        assert(swapchain != VK_NULL_HANDLE && "Get_handle() called on a moved-from or destroyed Vulkan_Swapchain");
        return swapchain;
    }

    // ---------- Get_image_format ----------
    VkFormat Vulkan_Swapchain::Get_image_format() const {
        assert(swapchain != VK_NULL_HANDLE && "Get_image_format() called on a moved-from or destroyed Vulkan_Swapchain");
        return image_format;
    }

    // ---------- Get_extent ----------
    VkExtent2D Vulkan_Swapchain::Get_extent() const {
        assert(swapchain != VK_NULL_HANDLE && "Get_extent() called on a moved-from or destroyed Vulkan_Swapchain");
        return extent;
    }

    // ---------- Get_image_views ----------
    const std::vector<VkImageView>& Vulkan_Swapchain::Get_image_views() const {
        assert(swapchain != VK_NULL_HANDLE && "Get_image_views() called on a moved-from or destroyed Vulkan_Swapchain");
        return image_views;
    }

    // ---------- Get_image_count ----------
    uint32_t Vulkan_Swapchain::Get_image_count() const {
        assert(swapchain != VK_NULL_HANDLE && "Get_image_count() called on a moved-from or destroyed Vulkan_Swapchain");
        return static_cast<uint32_t>(images.size());
    }

    // ---------- Get_present_mode ----------
    VkPresentModeKHR Vulkan_Swapchain::Get_present_mode() const {
        assert(swapchain != VK_NULL_HANDLE && "Get_present_mode() called on a moved-from or destroyed Vulkan_Swapchain");
        return selected_present_mode;
    }

    // ---------- Create_swapchain ----------
    void Vulkan_Swapchain::Create_swapchain(
        const Vulkan_Device& _device,
        const Vulkan_Surface& _surface,
        const Platform::Window& _window,
        uint32_t _preferred_image_count,
        bool _prefer_mailbox) {

        Swap_chain_support_details support = Query_swap_chain_support(physical_device_handle, surface_handle);

        if (support.formats.empty() || support.present_modes.empty()) {
            throw std::runtime_error("Swapchain is not supported adequately by this device/surface combination");
        }

        VkSurfaceFormatKHR surface_format = Choose_surface_format(support.formats);
        VkPresentModeKHR present_mode = Choose_present_mode(support.present_modes, _prefer_mailbox);
        VkExtent2D chosen_extent = Choose_extent(support.capabilities, _window);

        // Clamp the requested image count to what the surface actually
        // supports. maxImageCount == 0 means "no upper limit".
        uint32_t image_count = _preferred_image_count;
        if (support.capabilities.maxImageCount > 0) {
            image_count = std::min(image_count, support.capabilities.maxImageCount);
        }
        image_count = std::max(image_count, support.capabilities.minImageCount);

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = surface_handle;
        create_info.minImageCount = image_count;
        create_info.imageFormat = surface_format.format;
        create_info.imageColorSpace = surface_format.colorSpace;
        create_info.imageExtent = chosen_extent;
        create_info.imageArrayLayers = 1; // always 1 unless doing stereoscopic 3D rendering
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        uint32_t queue_family_indices_array[] = {
            queue_family_indices.graphics_family.value(),
            queue_family_indices.present_family.value()
        };

        // If graphics and present are different queue families, the
        // swapchain images need to be shared (CONCURRENT) between them.
        // If they're the same family, EXCLUSIVE is more efficient since
        // there's no actual sharing happening.
        if (queue_family_indices.graphics_family != queue_family_indices.present_family) {
            create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            create_info.queueFamilyIndexCount = 2;
            create_info.pQueueFamilyIndices = queue_family_indices_array;
        }
        else {
            create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            create_info.queueFamilyIndexCount = 0;
            create_info.pQueueFamilyIndices = nullptr;
        }

        // No special pre-transform (e.g. for rotated mobile displays) -
        // just use whatever the surface currently reports as its transform.
        create_info.preTransform = support.capabilities.currentTransform;

        // Ignore alpha blending with the OS window manager/desktop -
        // standard choice for a normal opaque game window.
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

        create_info.presentMode = present_mode;

        // Allows Vulkan to discard rendering operations on pixels that are
        // obscured by other windows on screen - minor performance gain,
        // no downside for a typical game window.
        create_info.clipped = VK_TRUE;

        // No previous swapchain to hand off resources from - this is the
        // initial creation, not a resize-triggered recreation.
        create_info.oldSwapchain = VK_NULL_HANDLE;

        VkResult result = vkCreateSwapchainKHR(device_handle, &create_info, nullptr, &swapchain);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create swapchain: " + Vulkan_utils::Vk_result_to_string(result)
            );
        }

        image_format = surface_format.format;
        extent = chosen_extent;
        selected_present_mode = present_mode;

        // Retrieve the actual images created by the swapchain. Note we
        // query the count again here - the driver is allowed to create
        // MORE images than minImageCount requested, so we can't just
        // assume image_count is the final number.
        uint32_t actual_image_count = 0;
        vkGetSwapchainImagesKHR(device_handle, swapchain, &actual_image_count, nullptr);
        images.resize(actual_image_count);
        vkGetSwapchainImagesKHR(device_handle, swapchain, &actual_image_count, images.data());

        std::cout << "[Vulkan_Swapchain] Swapchain created: " << extent.width << "x" << extent.height
            << ", " << actual_image_count << " images, present mode: "
            << (present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO") << "\n";
    }

    // ---------- Create_image_views ----------
    void Vulkan_Swapchain::Create_image_views() {
        image_views.resize(images.size());

        for (size_t i = 0; i < images.size(); ++i) {
            VkImageViewCreateInfo view_create_info{};
            view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_create_info.image = images[i];
            view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_create_info.format = image_format;

            // Identity swizzle - we don't want to remap color channels
            view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

            // These images are plain 2D color images - one mip level, one
            // array layer, used as the color aspect (not depth/stencil).
            view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_create_info.subresourceRange.baseMipLevel = 0;
            view_create_info.subresourceRange.levelCount = 1;
            view_create_info.subresourceRange.baseArrayLayer = 0;
            view_create_info.subresourceRange.layerCount = 1;

            VkResult result = vkCreateImageView(device_handle, &view_create_info, nullptr, &image_views[i]);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create swapchain image view: " + Vulkan_utils::Vk_result_to_string(result)
                );
            }
        }
    }

    // ---------- Query_swap_chain_support ----------
    Swap_chain_support_details Vulkan_Swapchain::Query_swap_chain_support(VkPhysicalDevice _physical_device, VkSurfaceKHR _surface) const {
        assert(_physical_device != VK_NULL_HANDLE && "Query_swap_chain_support() called with a null physical device");
        assert(_surface != VK_NULL_HANDLE && "Query_swap_chain_support() called with a null surface");

        Swap_chain_support_details details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physical_device, _surface, &details.capabilities);

        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(_physical_device, _surface, &format_count, nullptr);
        if (format_count > 0) {
            details.formats.resize(format_count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(_physical_device, _surface, &format_count, details.formats.data());
        }

        uint32_t present_mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(_physical_device, _surface, &present_mode_count, nullptr);
        if (present_mode_count > 0) {
            details.present_modes.resize(present_mode_count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(_physical_device, _surface, &present_mode_count, details.present_modes.data());
        }

        return details;
    }

    // ---------- Choose_surface_format ----------
    VkSurfaceFormatKHR Vulkan_Swapchain::Choose_surface_format(const std::vector<VkSurfaceFormatKHR>& _available_formats) const {
        assert(!_available_formats.empty() && "Choose_surface_format() called with an empty format list");

        // VK_FORMAT_B8G8R8A8_SRGB with SRGB_NONLINEAR color space is the
        // standard choice: 8 bits per channel, sRGB gamma correction
        // applied automatically by the hardware on write, which is what
        // you want for standard (non-HDR) color-correct rendering.
        for (const auto& format : _available_formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }

        // If our preferred format isn't available, just take whatever the
        // first one offered is - not ideal, but functional fallback.
        return _available_formats[0];
    }

    // ---------- Choose_present_mode ----------
    VkPresentModeKHR Vulkan_Swapchain::Choose_present_mode(const std::vector<VkPresentModeKHR>& _available_modes, bool _prefer_mailbox) const {
        assert(!_available_modes.empty() && "Choose_present_mode() called with an empty present mode list");

        if (_prefer_mailbox) {
            for (const auto& mode : _available_modes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                    return mode;
                }
            }
        }

        // VK_PRESENT_MODE_FIFO_KHR is guaranteed by the Vulkan spec to
        // always be supported, so this is a safe fallback that never fails.
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // ---------- Choose_extent ----------
    VkExtent2D Vulkan_Swapchain::Choose_extent(const VkSurfaceCapabilitiesKHR& _capabilities, const Platform::Window& _window) const {
        // If currentExtent isn't the special "any size" sentinel value
        // (UINT32_MAX), the surface dictates an exact size we must use -
        // typically matches the window size already, but some platforms
        // require using this value exactly instead of querying the window.
        if (_capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return _capabilities.currentExtent;
        }

        // Otherwise, we're free to choose - use the window's actual
        // framebuffer size, clamped to the min/max extent the surface allows.
        int width = 0, height = 0;
        _window.Get_framebuffer_size(width, height);

        VkExtent2D actual_extent{
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height)
        };

        actual_extent.width = std::clamp(
            actual_extent.width,
            _capabilities.minImageExtent.width,
            _capabilities.maxImageExtent.width
        );
        actual_extent.height = std::clamp(
            actual_extent.height,
            _capabilities.minImageExtent.height,
            _capabilities.maxImageExtent.height
        );

        return actual_extent;
    }

}