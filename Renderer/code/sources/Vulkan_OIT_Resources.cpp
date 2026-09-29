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
        : device_handle(_device),
        allocator(_allocator),
        current_extent(_extent),
        accumulation(Create_target(_device, _allocator, _extent, ACCUMULATION_FORMAT)),
        revealage(Create_target(_device, _allocator, _extent, REVEALAGE_FORMAT))
    {
        assert(allocator != VK_NULL_HANDLE && "Vulkan_Allocator must be fully constructed before creating the OIT targets");
        assert(_extent.width > 0 && _extent.height > 0 && "OIT target extent must be greater than zero");

        const double pixels = static_cast<double>(_extent.width) * static_cast<double>(_extent.height);

        std::cout << "[Vulkan_OIT_Resources] OIT targets created: " << _extent.width << "x" << _extent.height
            << " (accumulation " << (pixels * 8.0 / 1.0e6) << " MB, revealage " << (pixels * 2.0 / 1.0e6) << " MB, "
            << ((accumulation.lazily_allocated && revealage.lazily_allocated) ? "lazily allocated" : "device-local") << ").\n";
    }

    // ---------- Create_target ----------
    Vulkan_OIT_Resources::Target Vulkan_OIT_Resources::Create_target(VkDevice _device, VmaAllocator _allocator,
                                                                     VkExtent2D _extent, VkFormat _format)
    {
        assert(_allocator != VK_NULL_HANDLE && "Vulkan_Allocator must be fully constructed before creating the OIT targets");
        assert(_extent.width > 0 && _extent.height > 0 && "OIT target extent must be greater than zero");

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent = { _extent.width, _extent.height, 1 };
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = _format;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = OIT_TARGET_USAGE;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;

        Vulkan_Image_Utils::Image_Allocation created{};

        // Lazily allocated memory: on tile-based GPUs a transient
        // attachment then never gets backing memory at all. Desktop GPUs
        // expose no such memory type and VMA reports
        // VK_ERROR_FEATURE_NOT_PRESENT, in which case the target gets a
        // dedicated device-local allocation, as the depth buffer does.
        VmaAllocationCreateInfo lazy_info{};
        lazy_info.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;

        const bool lazily_allocated = vmaCreateImage(_allocator, &image_info, &lazy_info,
                                                     &created.image, &created.allocation, nullptr) == VK_SUCCESS;

        if (!lazily_allocated)
        {
            // The output of the failed call is not used.
            created = {};

            VmaAllocationCreateInfo device_info{};
            device_info.usage = VMA_MEMORY_USAGE_AUTO;
            device_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

            VK_CHECK(vmaCreateImage(_allocator, &image_info, &device_info, &created.image, &created.allocation, nullptr),
                "Vulkan_OIT_Resources: failed to create an OIT target");
        }

        // From here on the image is owned: if the view cannot be created,
        // the wrapper frees the image while the exception propagates.
        Target target;
        target.image = Unique_Image(_allocator, created);
        target.lazily_allocated = lazily_allocated;
        target.view = Unique_Image_View(_device, Vulkan_Image_Utils::Create_image_view(_device, target.image.Get(),
                                                                                      _format, VK_IMAGE_ASPECT_COLOR_BIT, 1));

        return target;
    }

    // ---------- Recreate ----------
    void Vulkan_OIT_Resources::Recreate(VkExtent2D _new_extent)
    {
        assert(accumulation.image && "Recreate() called on a moved-from Vulkan_OIT_Resources");
        assert(_new_extent.width > 0 && _new_extent.height > 0 && "Recreate() called with a zero extent");

        // The replacement pair is complete before anything is released: if
        // its creation throws, the current targets are still in place.
        Vulkan_OIT_Resources replacement(device_handle, allocator, _new_extent);

        *this = std::move(replacement);
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
