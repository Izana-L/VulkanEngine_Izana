#include <Vulkan_Depth_Resources.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer_System {

    // ---------- Constructor ----------
    Vulkan_Depth_Resources::Vulkan_Depth_Resources(
        const Vulkan_Device& _device,
        VmaAllocator _allocator,
        VkFormat _depth_format,
        VkExtent2D _extent)

        : device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator),
        depth_format(_depth_format),
        current_extent(_extent),
        depth_image_view(VK_NULL_HANDLE) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating depth resources");
        assert(allocator != VK_NULL_HANDLE && "Vulkan_Allocator must be fully constructed before creating depth resources");
        assert(_depth_format != VK_FORMAT_UNDEFINED && "Depth format cannot be VK_FORMAT_UNDEFINED");
        assert(_extent.width > 0 && _extent.height > 0 && "Depth resource extent must be greater than zero");

        Create(_extent);
    }

    // ---------- Destructor ----------
    Vulkan_Depth_Resources::~Vulkan_Depth_Resources() {
        Destroy();
    }

    // ---------- Destroy ----------
    void Vulkan_Depth_Resources::Destroy() {
        // Destroy in reverse order of creation: the view depends on the
        // image. The image and its memory now go together in one call.
        if (depth_image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_handle, depth_image_view, nullptr);
            depth_image_view = VK_NULL_HANDLE;
        }

        Vulkan_Image_Utils::Destroy_image(allocator, depth_image);
    }

    // ---------- Create ----------
    void Vulkan_Depth_Resources::Create(VkExtent2D _extent) {
        // ---------- Image creation ----------
        // OPTIMAL tiling lets the GPU arrange the image data however is
        // most efficient internally - we never need to read this layout
        // from the CPU, so there's no reason to use LINEAR tiling here.
        //
        // DEPTH_STENCIL_ATTACHMENT_BIT is required for an image used as a
        // depth/stencil attachment during rendering.
        //
        // One mip level: depth buffers don't need mipmaps.
        depth_image = Vulkan_Image_Utils::Create_image( allocator,_extent.width, _extent.height, 1,
                                                        depth_format, VK_IMAGE_TILING_OPTIMAL,
                                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,true);

        // ---------- Image view creation ----------
        // DEPTH_BIT here, not COLOR_BIT like the swapchain image views -
        // this view is for the depth aspect of the image, not color.
        // If the format includes a stencil component (e.g. D24_UNORM_S8_UINT),
        // you'd also include VK_IMAGE_ASPECT_STENCIL_BIT here once stencil
        // testing is actually used - left out for now since we don't use it yet.
        depth_image_view = Vulkan_Image_Utils::Create_image_view(device_handle, depth_image.image,
                                                                 depth_format,VK_IMAGE_ASPECT_DEPTH_BIT,1);

        current_extent = _extent;

        std::cout << "[Vulkan_Depth_Resources] Depth resources created: "
            << _extent.width << "x" << _extent.height << "\n";
    }

    // ---------- Recreate ----------
    void Vulkan_Depth_Resources::Recreate(VkExtent2D _new_extent) {
        assert(depth_image.image != VK_NULL_HANDLE && "Recreate() called on a moved-from or already-destroyed Vulkan_Depth_Resources");
        assert(_new_extent.width > 0 && _new_extent.height > 0 && "Recreate() called with a zero extent");

        Destroy();
        Create(_new_extent);

        std::cout << "[Vulkan_Depth_Resources] Depth resources recreated: "
            << _new_extent.width << "x" << _new_extent.height << "\n";
    }

    // ---------- Move constructor ----------
    Vulkan_Depth_Resources::Vulkan_Depth_Resources(Vulkan_Depth_Resources&& _other) noexcept
        : device_handle(_other.device_handle),
        allocator(_other.allocator),
        depth_format(_other.depth_format),
        current_extent(_other.current_extent),
        depth_image(_other.depth_image),
        depth_image_view(_other.depth_image_view) {

        _other.depth_image = {};
        _other.depth_image_view = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Depth_Resources& Vulkan_Depth_Resources::operator=(Vulkan_Depth_Resources&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            allocator = _other.allocator;
            depth_format = _other.depth_format;
            current_extent = _other.current_extent;
            depth_image = _other.depth_image;
            depth_image_view = _other.depth_image_view;

            _other.depth_image = {};
            _other.depth_image_view = VK_NULL_HANDLE;
        }
        return *this;
    }

    // ---------- Get_image_view ----------
    VkImageView Vulkan_Depth_Resources::Get_image_view() const {
        assert(depth_image_view != VK_NULL_HANDLE && "Get_image_view() called on a moved-from or destroyed Vulkan_Depth_Resources");
        return depth_image_view;
    }

    // ---------- Get_format ----------
    VkFormat Vulkan_Depth_Resources::Get_format() const {
        assert(depth_image_view != VK_NULL_HANDLE && "Get_format() called on a moved-from or destroyed Vulkan_Depth_Resources");
        return depth_format;
    }

}
