#include "Vulkan_Swapchain.hpp"
#include "Vulkan_Utils.hpp"

#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <limits>

namespace Renderer_System
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
        selected_present_mode(VK_PRESENT_MODE_FIFO_KHR)
    {
        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a swapchain");
        assert(surface_handle != VK_NULL_HANDLE && "Vulkan_Surface must be fully constructed before creating a swapchain");
        assert(_preferred_image_count >= 1 && "Preferred image count must be at least 1");
        assert(present_queue_handle != VK_NULL_HANDLE && "Vulkan_Device must have a valid present queue before creating a swapchain");

        Create_swapchain(_device, _surface, _window, _preferred_image_count, _prefer_mailbox);
        Create_image_views();
    }

    // ---------- Destructor ----------
    Vulkan_Swapchain::~Vulkan_Swapchain()
    {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Swapchain::Destroy()
    {
        for (VkImageView view : image_views)
            vkDestroyImageView(device_handle, view, nullptr);
        image_views.clear();

        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_handle, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
    }

    // ---------- Recreate ----------
    // Waits for a valid window size, destroys the old swapchain and
    // rebuilds it using the parameters already cached as members.
    // No parameters needed — everything was stored at construction time.
    void Vulkan_Swapchain::Recreate()
    {
        assert(swapchain != VK_NULL_HANDLE && "Recreate() called on a moved-from or already-destroyed Vulkan_Swapchain");

        // Handle minimization: wait until the window has a non-zero size.
        int width = 0, height = 0;
        window->Get_framebuffer_size(width, height);
        while (width == 0 || height == 0) {
            window->Get_framebuffer_size(width, height);
            glfwWaitEvents();
        }

        vkDeviceWaitIdle(device_handle);

        Destroy();
        Create_swapchain_internal();
        Create_image_views();

        std::cout << "[Vulkan_Swapchain] Swapchain recreated: "
            << extent.width << "x" << extent.height << "\n";
    }

    // ---------- Acquire_next_image ----------
    bool Vulkan_Swapchain::Acquire_next_image(
        VkSemaphore _image_available_semaphore,
        uint32_t& _out_image_index,
        uint64_t _timeout)
    {
        assert(swapchain != VK_NULL_HANDLE && "Acquire_next_image() called on a moved-from or destroyed Vulkan_Swapchain");
        assert(_image_available_semaphore != VK_NULL_HANDLE && "Acquire_next_image() called with a null semaphore");

        VkResult result = vkAcquireNextImageKHR(
            device_handle,
            swapchain,
            _timeout,
            _image_available_semaphore,
            VK_NULL_HANDLE,
            &_out_image_index
        );

        if (result == VK_ERROR_OUT_OF_DATE_KHR) return false;
        if (result == VK_SUBOPTIMAL_KHR)         return false;

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to acquire swapchain image: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        return true;
    }

    // ---------- Present ----------
    bool Vulkan_Swapchain::Present(
        VkSemaphore _render_finished_semaphore,
        uint32_t    _image_index,
        VkFence     _present_fence)
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

        VkSwapchainPresentFenceInfoEXT fence_info{};
        if (_present_fence != VK_NULL_HANDLE) {
            fence_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
            fence_info.swapchainCount = 1;
            fence_info.pFences = &_present_fence;
            present_info.pNext = &fence_info;
        }

        VkResult result = vkQueuePresentKHR(present_queue_handle, &present_info);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) return false;

        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to present swapchain image: " + Vulkan_Utils::Vk_result_to_string(result)
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
        selected_present_mode(_other.selected_present_mode)
    {
        _other.swapchain = VK_NULL_HANDLE;
        _other.image_views.clear();
    }

    // ---------- Move assignment ----------
    Vulkan_Swapchain& Vulkan_Swapchain::operator=(Vulkan_Swapchain&& _other) noexcept
    {
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

    // ---------- Getters ----------
    VkSwapchainKHR Vulkan_Swapchain::Get_handle() const
    {
        assert(swapchain != VK_NULL_HANDLE && "Get_handle() called on a moved-from or destroyed Vulkan_Swapchain");
        return swapchain;
    }

    VkFormat Vulkan_Swapchain::Get_image_format() const
    {
        assert(swapchain != VK_NULL_HANDLE && "Get_image_format() called on a moved-from or destroyed Vulkan_Swapchain");
        return image_format;
    }

    VkExtent2D Vulkan_Swapchain::Get_extent() const
    {
        assert(swapchain != VK_NULL_HANDLE && "Get_extent() called on a moved-from or destroyed Vulkan_Swapchain");
        return extent;
    }

    const std::vector<VkImageView>& Vulkan_Swapchain::Get_image_views() const
    {
        assert(swapchain != VK_NULL_HANDLE && "Get_image_views() called on a moved-from or destroyed Vulkan_Swapchain");
        return image_views;
    }

    uint32_t Vulkan_Swapchain::Get_image_count() const
    {
        assert(swapchain != VK_NULL_HANDLE && "Get_image_count() called on a moved-from or destroyed Vulkan_Swapchain");
        return static_cast<uint32_t>(images.size());
    }

    VkPresentModeKHR Vulkan_Swapchain::Get_present_mode() const
    {
        assert(swapchain != VK_NULL_HANDLE && "Get_present_mode() called on a moved-from or destroyed Vulkan_Swapchain");
        return selected_present_mode;
    }

    // ---------- Create_swapchain ----------
    // Public entry point: caches the parameters that Recreate will need,
    // then delegates to Create_swapchain_internal for the actual work.
    void Vulkan_Swapchain::Create_swapchain(
        const Vulkan_Device& _device,
        const Vulkan_Surface& _surface,
        const Platform::Window& _window,
        uint32_t                _preferred_image_count,
        bool                    _prefer_mailbox)
    {
        // All the handles needed by Create_swapchain_internal are already
        // cached as members in the constructor before this is called.
        // Nothing extra to cache here — delegate immediately.
        Create_swapchain_internal();
    }

    // ---------- Create_swapchain_internal ----------
    // Core creation logic. Uses only cached members so both the initial
    // creation path and Recreate() can call it without parameters.
    void Vulkan_Swapchain::Create_swapchain_internal()
    {
        Swap_chain_support_details support =
            Query_swap_chain_support(physical_device_handle, surface_handle);

        if (support.formats.empty() || support.present_modes.empty()) {
            throw std::runtime_error(
                "Swapchain is not supported adequately by this device/surface combination"
            );
        }

        VkSurfaceFormatKHR surface_format = Choose_surface_format(support.formats);
        VkPresentModeKHR   present_mode = Choose_present_mode(support.present_modes, prefer_mailbox);
        VkExtent2D         chosen_extent = Choose_extent(support.capabilities, *window);

        uint32_t image_count = preferred_image_count;
        if (support.capabilities.maxImageCount > 0)
            image_count = std::min(image_count, support.capabilities.maxImageCount);
        image_count = std::max(image_count, support.capabilities.minImageCount);

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = surface_handle;
        create_info.minImageCount = image_count;
        create_info.imageFormat = surface_format.format;
        create_info.imageColorSpace = surface_format.colorSpace;
        create_info.imageExtent = chosen_extent;
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
                "Failed to create swapchain: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        image_format = surface_format.format;
        extent = chosen_extent;
        selected_present_mode = present_mode;

        uint32_t actual_image_count = 0;
        vkGetSwapchainImagesKHR(device_handle, swapchain, &actual_image_count, nullptr);
        images.resize(actual_image_count);
        vkGetSwapchainImagesKHR(device_handle, swapchain, &actual_image_count, images.data());

        std::cout << "[Vulkan_Swapchain] Swapchain created: "
            << extent.width << "x" << extent.height
            << ", " << actual_image_count << " images, present mode: "
            << (present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO") << "\n";

       // Negotiated color format: decides WHO applies the gamma curve.
       // An *_SRGB format means the hardware encodes linear -> sRGB when
       // writing the attachment, so the fragment shader must output LINEAR.
        std::cout << "[Vulkan_Swapchain] Surface format: "
            << Vulkan_Utils::Vk_format_to_string(image_format)
            << (Vulkan_Utils::Is_srgb_format(image_format)
                ? "  (hardware encodes linear -> sRGB: shader must output LINEAR)"
                : "  (no hardware encode: shader must apply gamma)")
            << ", color space: "
            << (surface_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
                ? "SRGB_NONLINEAR" : "non-standard")
            << "\n";
    }

    // ---------- Create_image_views ----------
    void Vulkan_Swapchain::Create_image_views()
    {
        image_views.resize(images.size());

        for (size_t i = 0; i < images.size(); ++i) {
            VkImageViewCreateInfo view_create_info{};
            view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_create_info.image = images[i];
            view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_create_info.format = image_format;
            view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_create_info.subresourceRange.baseMipLevel = 0;
            view_create_info.subresourceRange.levelCount = 1;
            view_create_info.subresourceRange.baseArrayLayer = 0;
            view_create_info.subresourceRange.layerCount = 1;

            VkResult result = vkCreateImageView(device_handle, &view_create_info, nullptr, &image_views[i]);
            if (result != VK_SUCCESS) {
                throw std::runtime_error(
                    "Failed to create swapchain image view: " + Vulkan_Utils::Vk_result_to_string(result)
                );
            }
        }
    }

    // ---------- Query_swap_chain_support ----------
    Swap_chain_support_details Vulkan_Swapchain::Query_swap_chain_support(
        VkPhysicalDevice _physical_device,
        VkSurfaceKHR     _surface) const
    {
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
    VkSurfaceFormatKHR Vulkan_Swapchain::Choose_surface_format(
        const std::vector<VkSurfaceFormatKHR>& _available_formats) const
    {
        assert(!_available_formats.empty() && "Choose_surface_format() called with an empty format list");

        for (const auto& format : _available_formats) 
        {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;
        }

        // 2nd choice: any other sRGB format. The shaders output linear color
         // and depend on the hardware encode, so what matters is keeping an
         // *_SRGB format, not the exact channel order.
        for (const auto& format : _available_formats) 
        {
            if (Vulkan_Utils::Is_srgb_format(format.format) &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;
        }

        // Fallback: no sRGB format at all. Nothing will apply the gamma curve
        // and the image will look washed out - warn instead of failing silently.
        std::cout << "[Vulkan_Swapchain] WARNING: no sRGB surface format available, "
            << "falling back to "
            << Vulkan_Utils::Vk_format_to_string(_available_formats[0].format)
            << ". Shader output would need manual gamma correction.\n";

        return _available_formats[0];
    }

    // ---------- Choose_present_mode ----------
    VkPresentModeKHR Vulkan_Swapchain::Choose_present_mode(
        const std::vector<VkPresentModeKHR>& _available_modes,
        bool _prefer_mailbox) const
    {
        assert(!_available_modes.empty() && "Choose_present_mode() called with an empty present mode list");

        if (_prefer_mailbox) {
            for (const auto& mode : _available_modes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                    return mode;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // ---------- Choose_extent ----------
    VkExtent2D Vulkan_Swapchain::Choose_extent(
        const VkSurfaceCapabilitiesKHR& _capabilities,
        const Platform::Window& _window) const
    {
        if (_capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
            return _capabilities.currentExtent;

        int width = 0, height = 0;
        _window.Get_framebuffer_size(width, height);

        VkExtent2D actual_extent{
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height)
        };

        actual_extent.width = std::clamp(actual_extent.width,
            _capabilities.minImageExtent.width,
            _capabilities.maxImageExtent.width);
        actual_extent.height = std::clamp(actual_extent.height,
            _capabilities.minImageExtent.height,
            _capabilities.maxImageExtent.height);

        return actual_extent;
    }

} // namespace Renderer