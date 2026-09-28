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
    }

    // ---------- Constructor ----------
    Vulkan_OIT_Resources::Vulkan_OIT_Resources(const Vulkan_Device& _device, VmaAllocator _allocator, VkExtent2D _extent)
        : device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator),
        current_extent(_extent)
    {
        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating the OIT targets");
        assert(allocator != VK_NULL_HANDLE && "Vulkan_Allocator must be fully constructed before creating the OIT targets");
        assert(_extent.width > 0 && _extent.height > 0 && "OIT target extent must be greater than zero");

        // Enforced in every build, like every other format requirement of
        // the engine: both are mandatory formats for these features, so a
        // failure means a non-conformant device.
        Vulkan_Image_Utils::Require_optimal_tiling_features(_device, ACCUMULATION_FORMAT, OIT_TARGET_FEATURES, "Vulkan_OIT_Resources (accumulation)");
        Vulkan_Image_Utils::Require_optimal_tiling_features(_device, REVEALAGE_FORMAT, OIT_TARGET_FEATURES, "Vulkan_OIT_Resources (revealage)");

        Create(_extent);
    }

    // ---------- Destructor ----------
    Vulkan_OIT_Resources::~Vulkan_OIT_Resources()
    {
        Destroy();
    }

    // ---------- Create_target ----------
    Vulkan_OIT_Resources::Target Vulkan_OIT_Resources::Create_target(VkExtent2D _extent, VkFormat _format,
                                                                     bool& _out_lazily_allocated) const
    {
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

        Target target{};

        // Lazily allocated memory: on tile-based GPUs a transient
        // attachment then never gets backing memory at all. Desktop GPUs
        // expose no such memory type and VMA reports
        // VK_ERROR_FEATURE_NOT_PRESENT, in which case the target gets a
        // dedicated device-local allocation, as the depth buffer does.
        VmaAllocationCreateInfo lazy_info{};
        lazy_info.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;

        _out_lazily_allocated = vmaCreateImage(allocator, &image_info, &lazy_info,
                                               &target.image.image, &target.image.allocation, nullptr) == VK_SUCCESS;

        if (!_out_lazily_allocated)
        {
            target.image = {};

            VmaAllocationCreateInfo device_info{};
            device_info.usage = VMA_MEMORY_USAGE_AUTO;
            device_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

            VK_CHECK(vmaCreateImage(allocator, &image_info, &device_info, &target.image.image, &target.image.allocation, nullptr),
                "Vulkan_OIT_Resources: failed to create an OIT target");
        }

        try
        {
            target.view = Vulkan_Image_Utils::Create_image_view(device_handle, target.image.image, _format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }
        catch (...)
        {
            Vulkan_Image_Utils::Destroy_image(allocator, target.image);
            throw;
        }

        return target;
    }

    // ---------- Create ----------
    void Vulkan_OIT_Resources::Create(VkExtent2D _extent)
    {
        bool accumulation_lazy = false;
        bool revealage_lazy = false;

        accumulation = Create_target(_extent, ACCUMULATION_FORMAT, accumulation_lazy);

        try
        {
            revealage = Create_target(_extent, REVEALAGE_FORMAT, revealage_lazy);
        }
        catch (...)
        {
            Destroy_target(device_handle, allocator, accumulation);
            throw;
        }

        current_extent = _extent;

        const double pixels = static_cast<double>(_extent.width) * static_cast<double>(_extent.height);

        std::cout << "[Vulkan_OIT_Resources] OIT targets created: " << _extent.width << "x" << _extent.height
            << " (accumulation " << (pixels * 8.0 / 1.0e6) << " MB, revealage " << (pixels * 2.0 / 1.0e6) << " MB, "
            << ((accumulation_lazy && revealage_lazy) ? "lazily allocated" : "device-local") << ").\n";
    }

    // ---------- Destroy ----------
    void Vulkan_OIT_Resources::Destroy_target(VkDevice _device, VmaAllocator _allocator, Target& _target)
    {
        // The view depends on the image: destroyed first.
        if (_target.view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(_device, _target.view, nullptr);
            _target.view = VK_NULL_HANDLE;
        }

        Vulkan_Image_Utils::Destroy_image(_allocator, _target.image);
    }

    void Vulkan_OIT_Resources::Destroy()
    {
        Destroy_target(device_handle, allocator, revealage);
        Destroy_target(device_handle, allocator, accumulation);
    }

    // ---------- Recreate ----------
    void Vulkan_OIT_Resources::Recreate(VkExtent2D _new_extent)
    {
        assert(accumulation.image.image != VK_NULL_HANDLE && "Recreate() called on a moved-from or destroyed Vulkan_OIT_Resources");
        assert(_new_extent.width > 0 && _new_extent.height > 0 && "Recreate() called with a zero extent");

        Destroy();
        Create(_new_extent);
    }

    // ---------- Move constructor ----------
    Vulkan_OIT_Resources::Vulkan_OIT_Resources(Vulkan_OIT_Resources&& _other) noexcept
        : device_handle(_other.device_handle),
        allocator(_other.allocator),
        current_extent(_other.current_extent),
        accumulation(_other.accumulation),
        revealage(_other.revealage)
    {
        _other.accumulation = {};
        _other.revealage = {};
    }

    // ---------- Move assignment ----------
    Vulkan_OIT_Resources& Vulkan_OIT_Resources::operator=(Vulkan_OIT_Resources&& _other) noexcept
    {
        if (this != &_other)
        {
            Destroy();

            device_handle = _other.device_handle;
            allocator = _other.allocator;
            current_extent = _other.current_extent;
            accumulation = _other.accumulation;
            revealage = _other.revealage;

            _other.accumulation = {};
            _other.revealage = {};
        }
        return *this;
    }

    // ---------- Getters ----------
    VkImageView Vulkan_OIT_Resources::Get_accumulation_view() const
    {
        assert(accumulation.view != VK_NULL_HANDLE && "Get_accumulation_view() called on a moved-from or destroyed Vulkan_OIT_Resources");
        return accumulation.view;
    }

    VkImageView Vulkan_OIT_Resources::Get_revealage_view() const
    {
        assert(revealage.view != VK_NULL_HANDLE && "Get_revealage_view() called on a moved-from or destroyed Vulkan_OIT_Resources");
        return revealage.view;
    }

    VkImage Vulkan_OIT_Resources::Get_accumulation_image() const
    {
        return accumulation.image.image;
    }

    VkImage Vulkan_OIT_Resources::Get_revealage_image() const
    {
        return revealage.image.image;
    }

} // namespace Renderer_System
