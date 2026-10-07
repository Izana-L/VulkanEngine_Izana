#include <Vulkan_Depth_Resources.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer_System {

    namespace
    {
        // ---------- Image creation ----------
        // OPTIMAL tiling lets the GPU arrange the image data however is
        // most efficient internally - we never need to read this layout
        // from the CPU, so there's no reason to use LINEAR tiling here.
        //
        // DEPTH_STENCIL_ATTACHMENT_BIT is required for an image used as a
        // depth/stencil attachment during rendering.
        //
        // One mip level: depth buffers don't need mipmaps.
        Vulkan_Image_Utils::Image_Allocation Create_depth_image(VmaAllocator _allocator, VkFormat _depth_format, VkExtent2D _extent)
        {
            assert(_allocator != VK_NULL_HANDLE && "Vulkan_Allocator must be fully constructed before creating depth resources");
            assert(_depth_format != VK_FORMAT_UNDEFINED && "Depth format cannot be VK_FORMAT_UNDEFINED");
            assert(_extent.width > 0 && _extent.height > 0 && "Depth resource extent must be greater than zero");

            return Vulkan_Image_Utils::Create_image(_allocator, _extent.width, _extent.height, 1,
                                                    _depth_format, VK_IMAGE_TILING_OPTIMAL,
                                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, true);
        }
    }

    // ---------- Constructors ----------
    Vulkan_Depth_Resources::Vulkan_Depth_Resources(
        const Vulkan_Device& _device,
        VmaAllocator _allocator,
        VkFormat _depth_format,
        VkExtent2D _extent)

        : Vulkan_Depth_Resources(_device.Get_logical_device_handle(), _allocator, _depth_format, _extent) {
    }

    Vulkan_Depth_Resources::Vulkan_Depth_Resources(
        VkDevice _device,
        VmaAllocator _allocator,
        VkFormat _depth_format,
        VkExtent2D _extent)

        : device_handle(_device),
        allocator(_allocator),
        depth_format(_depth_format),
        current_extent(_extent),
        depth_image(_allocator, Create_depth_image(_allocator, _depth_format, _extent)),

        // ---------- Image view creation ----------
        // DEPTH_BIT only, not COLOR_BIT like the swapchain image views.
        // This is correct even when the format has a stencil aspect
        // (D32_SFLOAT_S8_UINT): as a framebuffer attachment the
        // specification ignores the view's aspectMask and uses every aspect
        // the format has, and a view that is sampled must have exactly one
        // aspect. Stencil testing is not used; enabling it would need a
        // format with a stencil aspect chosen on purpose, stencilLoadOp /
        // stencilStoreOp in the render pass, barriers on the image with
        // both aspects, and a separate STENCIL view if the stencil is ever
        // sampled.
        depth_image_view(Create_unique_image_view(_device, depth_image.Get(),
                                                  _depth_format, VK_IMAGE_ASPECT_DEPTH_BIT, 1)) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating depth resources");

        std::cout << "[Vulkan_Depth_Resources] Depth resources created: "
            << _extent.width << "x" << _extent.height << "\n";
    }

    // ---------- Recreate ----------
    void Vulkan_Depth_Resources::Recreate(VkExtent2D _new_extent) {
        assert(depth_image && "Recreate() called on a moved-from Vulkan_Depth_Resources");
        assert(_new_extent.width > 0 && _new_extent.height > 0 && "Recreate() called with a zero extent");

        // The replacement is complete before anything is released: if its
        // creation throws, the current image and view are still in place.
        Vulkan_Depth_Resources replacement(device_handle, allocator, depth_format, _new_extent);

        *this = std::move(replacement);
    }

    // ---------- Get_image_view ----------
    VkImageView Vulkan_Depth_Resources::Get_image_view() const {
        assert(depth_image_view && "Get_image_view() called on a moved-from Vulkan_Depth_Resources");
        return depth_image_view.Get();
    }

    // ---------- Get_format ----------
    VkFormat Vulkan_Depth_Resources::Get_format() const {
        assert(depth_image_view && "Get_format() called on a moved-from Vulkan_Depth_Resources");
        return depth_format;
    }

}
