#include <Vulkan_OIT_Resources.hpp>
#include <Vulkan_Utils.hpp>

#include <cassert>
#include <iostream>

namespace Renderer_System
{

    namespace
    {
        // Written by the transparent subpass, read by the composite subpass
        // of the same render pass, never outside it.
        constexpr VkImageUsageFlags OIT_TARGET_USAGE = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                       VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                                       VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

        // Both formats are blended into and read back as input attachments.
        constexpr VkFormatFeatureFlags OIT_TARGET_FEATURES = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;

        // Enforced in every build, like every other format requirement of
        // the engine: both are mandatory formats for these features, so a
        // failure means a non-conformant device. Returns the device handle
        // the targets are created on.
        VkDevice Require_target_formats(const Vulkan_Device& _device)
        {
            assert(_device.Get_logical_device_handle() != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating the OIT targets");

            Vulkan_Image_Utils::Require_optimal_tiling_features(_device, Vulkan_OIT_Resources::ACCUMULATION_FORMAT, OIT_TARGET_FEATURES, "Vulkan_OIT_Resources (accumulation)");
            Vulkan_Image_Utils::Require_optimal_tiling_features(_device, Vulkan_OIT_Resources::REVEALAGE_FORMAT, OIT_TARGET_FEATURES, "Vulkan_OIT_Resources (revealage)");

            return _device.Get_logical_device_handle();
        }
    }

    // ---------- Constructors ----------
    Vulkan_OIT_Resources::Vulkan_OIT_Resources(const Vulkan_Device& _device, VmaAllocator _allocator, VkExtent2D _extent)
        : Vulkan_OIT_Resources(Require_target_formats(_device), _allocator, _extent)
    {
    }

    Vulkan_OIT_Resources::Vulkan_OIT_Resources(VkDevice _device, VmaAllocator _allocator, VkExtent2D _extent)
        : accumulation(Create_target(_device, _allocator, _extent, ACCUMULATION_FORMAT)),
        revealage(Create_target(_device, _allocator, _extent, REVEALAGE_FORMAT))
    {
        const double pixels = static_cast<double>(_extent.width) * static_cast<double>(_extent.height);

        std::cout << "[Vulkan_OIT_Resources] OIT targets created: " << _extent.width << "x" << _extent.height
            << " (accumulation " << (pixels * 8.0 / 1.0e6) << " MB, revealage " << (pixels * 2.0 / 1.0e6) << " MB, "
            << ((accumulation.lazily_allocated && revealage.lazily_allocated) ? "lazily allocated" : "device-local") << ").\n";
    }

    // ---------- Create_target ----------
    Vulkan_OIT_Resources::Target Vulkan_OIT_Resources::Create_target(VkDevice _device, VmaAllocator _allocator,
                                                                     VkExtent2D _extent, VkFormat _format)
    {
        // Create_image checks, in every build and before anything is
        // created, that the allocator exists and that the device can make
        // an image of this size, format and usage. The memory is lazily
        // allocated where the device has such a memory type (a transient
        // attachment then never gets backing memory on a tile-based GPU),
        // dedicated device-local memory otherwise, as for the depth buffer.
        const Vulkan_Image_Utils::Image_Allocation created = Vulkan_Image_Utils::Create_image(
            _allocator, _extent.width, _extent.height, 1, _format, VK_IMAGE_TILING_OPTIMAL, OIT_TARGET_USAGE,
            Vulkan_Image_Utils::Image_Memory::Lazy_Or_Dedicated);

        // From here on the image is owned: if the view cannot be created,
        // the wrapper frees the image while the exception propagates.
        Target target;
        target.image = Unique_Image(_allocator, created);
        target.lazily_allocated = created.lazily_allocated;
        target.view = Create_unique_image_view(_device, target.image.Get(), _format, VK_IMAGE_ASPECT_COLOR_BIT, 1);

        return target;
    }

    // ---------- Getters ----------
    VkImageView Vulkan_OIT_Resources::Get_accumulation_view() const
    {
        assert(accumulation.view && "Get_accumulation_view() called on a moved-from Vulkan_OIT_Resources");
        return accumulation.view.Get();
    }

    VkImageView Vulkan_OIT_Resources::Get_revealage_view() const
    {
        assert(revealage.view && "Get_revealage_view() called on a moved-from Vulkan_OIT_Resources");
        return revealage.view.Get();
    }

    VkImage Vulkan_OIT_Resources::Get_accumulation_image() const
    {
        return accumulation.image.Get();
    }

    VkImage Vulkan_OIT_Resources::Get_revealage_image() const
    {
        return revealage.image.Get();
    }

} // namespace Renderer_System
