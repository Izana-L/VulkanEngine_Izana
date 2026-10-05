#include "Vulkan_Swapchain.hpp"
#include "Vulkan_Utils.hpp"

#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <limits>

namespace Renderer_System
{

    // ---------- Constructors ----------
    Vulkan_Swapchain::Vulkan_Swapchain( const Vulkan_Device& _device,const Vulkan_Surface& _surface,
                                        VkExtent2D _desired_extent,uint32_t _preferred_image_count,bool _prefer_mailbox)
                                        : Vulkan_Swapchain(_device.Get_logical_device_handle(),
                                                           _device.Get_physical_device_handle(),
                                                           _surface.Get_handle(),
                                                           _device.Get_queue_family_indices(),
                                                           _preferred_image_count,
                                                           _prefer_mailbox,
                                                           _desired_extent,
                                                           VK_NULL_HANDLE)
    {
    }

    Vulkan_Swapchain::Vulkan_Swapchain( VkDevice _device,VkPhysicalDevice _physical_device,VkSurfaceKHR _surface,
                                        const Queue_Family_Indices& _queue_family_indices,uint32_t _preferred_image_count,
                                        bool _prefer_mailbox,VkExtent2D _desired_extent,VkSwapchainKHR _old_swapchain)
                                        : device_handle(_device),
                                        physical_device_handle(_physical_device),
                                        surface_handle(_surface),
                                        preferred_image_count(_preferred_image_count),
                                        prefer_mailbox(_prefer_mailbox),
                                        queue_family_indices(_queue_family_indices),
                                        image_format(VK_FORMAT_UNDEFINED),
                                        extent{ 0, 0 },
                                        selected_present_mode(VK_PRESENT_MODE_FIFO_KHR),
                                        retired(false)
    {
        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a swapchain");
        assert(surface_handle != VK_NULL_HANDLE && "Vulkan_Surface must be fully constructed before creating a swapchain");
        assert(_preferred_image_count >= 1 && "Preferred image count must be at least 1");

        Create_swapchain(_desired_extent, _old_swapchain);
        Create_image_views();
    }

    // ---------- Move assignment ----------
    Vulkan_Swapchain& Vulkan_Swapchain::operator=(Vulkan_Swapchain&& _other) noexcept
    {
        if (this != &_other) {
            // The image views look at the images of the current swapchain:
            // they go first, then the swapchain.
            image_views.clear();
            images.clear();
            swapchain.Reset();

            device_handle = _other.device_handle;
            physical_device_handle = _other.physical_device_handle;
            surface_handle = _other.surface_handle;
            preferred_image_count = _other.preferred_image_count;
            prefer_mailbox = _other.prefer_mailbox;
            queue_family_indices = _other.queue_family_indices;
            image_format = _other.image_format;
            extent = _other.extent;
            selected_present_mode = _other.selected_present_mode;
            retired = _other.retired;

            swapchain = std::move(_other.swapchain);
            images = std::move(_other.images);
            image_views = std::move(_other.image_views);

            _other.images.clear();
            _other.image_views.clear();
        }
        return *this;
    }

    // ---------- Can_recreate ----------
    bool Vulkan_Swapchain::Can_recreate(VkExtent2D _desired_extent) const
    {
        assert(device_handle != VK_NULL_HANDLE && "Can_recreate() called on a moved-from Vulkan_Swapchain");

        VkSurfaceCapabilitiesKHR capabilities{};
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_handle, surface_handle, &capabilities),
            "Vulkan_Swapchain: query surface capabilities");

        const VkExtent2D chosen = Choose_extent(capabilities, _desired_extent);

        return chosen.width > 0 && chosen.height > 0;
    }

    // ---------- Recreate ----------
    // Builds the replacement with the current swapchain as oldSwapchain and
    // swaps it in. Never waits: the caller has already made sure that
    // nothing pending uses the current images.
    bool Vulkan_Swapchain::Recreate(VkExtent2D _desired_extent)
    {
        assert(device_handle != VK_NULL_HANDLE && "Recreate() called on a moved-from Vulkan_Swapchain");

        // A surface without area cannot hold a swapchain. Nothing has been
        // touched, so the current swapchain stays as it is.
        if (!Can_recreate(_desired_extent))
            return false;

        // The swapchain left behind by a failed creation is retired and
        // cannot be the oldSwapchain of a new one: it is destroyed first and
        // the creation starts from scratch.
        if (retired) {
            image_views.clear();
            images.clear();
            swapchain.Reset();
            retired = false;
        }

        // From the moment it is passed as oldSwapchain the current swapchain
        // is retired, even if the creation fails: recorded before the call.
        VkSwapchainKHR old_swapchain = swapchain.Get();
        retired = old_swapchain != VK_NULL_HANDLE;

        Vulkan_Swapchain replacement(device_handle, physical_device_handle, surface_handle, queue_family_indices,
                                     preferred_image_count, prefer_mailbox, _desired_extent, old_swapchain);

        // Destroys the old image views and the old swapchain, and clears
        // the retired flag (the replacement is a fresh swapchain).
        *this = std::move(replacement);

        std::cout << "[Vulkan_Swapchain] Swapchain recreated: "
            << extent.width << "x" << extent.height << "\n";

        return true;
    }

    // ---------- Getters ----------
    VkSwapchainKHR Vulkan_Swapchain::Get_handle() const
    {
        assert(swapchain && "Get_handle() called on a moved-from or destroyed Vulkan_Swapchain");
        return swapchain.Get();
    }

    VkFormat Vulkan_Swapchain::Get_image_format() const
    {
        return image_format;
    }

    VkExtent2D Vulkan_Swapchain::Get_extent() const
    {
        return extent;
    }

    VkImageView Vulkan_Swapchain::Get_image_view(uint32_t _index) const
    {
        assert(_index < image_views.size() && "Get_image_view() called with an out-of-range index or on a destroyed Vulkan_Swapchain");
        return image_views[_index].Get();
    }

    uint32_t Vulkan_Swapchain::Get_image_count() const
    {
        return static_cast<uint32_t>(images.size());
    }

    VkPresentModeKHR Vulkan_Swapchain::Get_present_mode() const
    {
        return selected_present_mode;
    }

    // ---------- Create_swapchain ----------
    // Core creation logic, shared by the constructor and Recreate() through
    // the private constructor. Uses the cached members plus the two
    // arguments that change between creations.
    void Vulkan_Swapchain::Create_swapchain(VkExtent2D _desired_extent, VkSwapchainKHR _old_swapchain)
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
        VkExtent2D         chosen_extent = Choose_extent(support.capabilities, _desired_extent);

        if (chosen_extent.width == 0 || chosen_extent.height == 0) {
            throw std::runtime_error("Vulkan_Swapchain: the surface has no area (minimized window), a swapchain cannot be created");
        }

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

        // The previous swapchain lets the driver reuse its resources and
        // gives the transition to the new one; it is retired by the call
        // whether the creation succeeds or not.
        create_info.oldSwapchain = _old_swapchain;

        // The wrapper takes the handle only after VK_SUCCESS.
        VkSwapchainKHR created = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSwapchainKHR(device_handle, &create_info, nullptr, &created),
            "Failed to create swapchain");

        swapchain = Unique_Swapchain(device_handle, created);

        image_format = surface_format.format;
        extent = chosen_extent;
        selected_present_mode = present_mode;

        uint32_t actual_image_count = 0;
        VK_CHECK(vkGetSwapchainImagesKHR(device_handle, swapchain.Get(), &actual_image_count, nullptr),
            "Vulkan_Swapchain: query swapchain images");
        images.resize(actual_image_count);
        VK_CHECK(vkGetSwapchainImagesKHR(device_handle, swapchain.Get(), &actual_image_count, images.data()),
            "Vulkan_Swapchain: query swapchain images");

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
        // Built in a local vector so that a failure on view i releases the
        // views before it; the members are replaced once all exist. The
        // capacity is reserved up front, so appending a wrapper cannot
        // throw between the creation of a view and its ownership.
        std::vector<Unique_Image_View> created;
        created.reserve(images.size());

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

            VkImageView view = VK_NULL_HANDLE;
            VK_CHECK(vkCreateImageView(device_handle, &view_create_info, nullptr, &view),
                "Failed to create swapchain image view");

            created.emplace_back(device_handle, view);
        }

        image_views = std::move(created);
    }

    // ---------- Query_swap_chain_support ----------
    Swap_chain_support_details Vulkan_Swapchain::Query_swap_chain_support(
        VkPhysicalDevice _physical_device,
        VkSurfaceKHR     _surface) const
    {
        assert(_physical_device != VK_NULL_HANDLE && "Query_swap_chain_support() called with a null physical device");
        assert(_surface != VK_NULL_HANDLE && "Query_swap_chain_support() called with a null surface");

        Swap_chain_support_details details;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physical_device, _surface, &details.capabilities),
            "Vulkan_Swapchain: query surface capabilities");

        uint32_t format_count = 0;
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(_physical_device, _surface, &format_count, nullptr),
            "Vulkan_Swapchain: query surface formats");
        if (format_count > 0) {
            details.formats.resize(format_count);
            VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(_physical_device, _surface, &format_count, details.formats.data()),
                "Vulkan_Swapchain: query surface formats");
        }

        uint32_t present_mode_count = 0;
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(_physical_device, _surface, &present_mode_count, nullptr),
            "Vulkan_Swapchain: query surface present modes");
        if (present_mode_count > 0) {
            details.present_modes.resize(present_mode_count);
            VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(_physical_device, _surface, &present_mode_count, details.present_modes.data()),
                "Vulkan_Swapchain: query surface present modes");
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
        VkExtent2D _desired_extent) const
    {
        if (_capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
            return _capabilities.currentExtent;

        // The surface leaves the size to the swapchain and the caller has
        // none to offer: the clamp below would invent a minimum-size
        // swapchain for a window that has no area.
        if (_desired_extent.width == 0 || _desired_extent.height == 0)
            return { 0, 0 };

        VkExtent2D actual_extent = _desired_extent;

        actual_extent.width = std::clamp(actual_extent.width,
            _capabilities.minImageExtent.width,
            _capabilities.maxImageExtent.width);
        actual_extent.height = std::clamp(actual_extent.height,
            _capabilities.minImageExtent.height,
            _capabilities.maxImageExtent.height);

        return actual_extent;
    }

} // namespace Renderer
