#include <Vulkan_Depth_Resources.hpp>
#include <Vulkan_Utils.hpp>

#include <stdexcept>
#include <iostream>
#include <cassert>

namespace Renderer_System {

    // ---------- Constructor ----------
    Vulkan_Depth_Resources::Vulkan_Depth_Resources(
        const Vulkan_Device& _device,
        VkFormat _depth_format,
        VkExtent2D _extent)

        : device_handle(_device.Get_logical_device_handle()),
        device(&_device),
        depth_format(_depth_format),
        current_extent(_extent),
        depth_image(VK_NULL_HANDLE),
        depth_image_memory(VK_NULL_HANDLE),
        depth_image_view(VK_NULL_HANDLE) {

        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating depth resources");
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
        // image, and the image depends on the memory being bound to it.
        if (depth_image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_handle, depth_image_view, nullptr);
            depth_image_view = VK_NULL_HANDLE;
        }

        if (depth_image != VK_NULL_HANDLE) {
            vkDestroyImage(device_handle, depth_image, nullptr);
            depth_image = VK_NULL_HANDLE;
        }

        if (depth_image_memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_handle, depth_image_memory, nullptr);
            depth_image_memory = VK_NULL_HANDLE;
        }
    }

    // ---------- Create ----------
    void Vulkan_Depth_Resources::Create(VkExtent2D _extent) {
        // ---------- Image creation ----------
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent.width = _extent.width;
        image_info.extent.height = _extent.height;
        image_info.extent.depth = 1; // always 1 for a 2D image
        image_info.mipLevels = 1;     // depth buffers don't need mipmaps
        image_info.arrayLayers = 1;
        image_info.format = depth_format;

        // OPTIMAL tiling lets the GPU arrange the image data however is
        // most efficient internally - we never need to read this layout
        // from the CPU, so there's no reason to use LINEAR tiling here.
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;

        // UNDEFINED is correct for a freshly created image with no data
        // to preserve - we're about to clear it on first use anyway.
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // This image will be used as a depth/stencil attachment during
        // rendering - this usage flag is required for that purpose.
        image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        image_info.samples = VK_SAMPLE_COUNT_1_BIT; // no MSAA for now, matches the render pass
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE; // only used by the graphics queue

        VkResult result = vkCreateImage(device_handle, &image_info, nullptr, &depth_image);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create depth image: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        // ---------- Memory allocation ----------
        // Ask Vulkan how much memory this specific image needs, and which
        // memory types are compatible with it (alignment, size, etc. can
        // vary by GPU/driver).
        VkMemoryRequirements memory_requirements{};
        vkGetImageMemoryRequirements(device_handle, depth_image, &memory_requirements);

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = memory_requirements.size;

        // DEVICE_LOCAL_BIT requests memory that's physically on the GPU -
        // the fastest option, appropriate since the CPU never needs to
        // read or write this depth data directly.
        alloc_info.memoryTypeIndex = device->Find_memory_type(
            memory_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );

        result = vkAllocateMemory(device_handle, &alloc_info, nullptr, &depth_image_memory);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to allocate depth image memory: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        // Bind the allocated memory to the image - before this call, the
        // image exists but has no actual storage backing it.
        // The "0" is the offset within the allocated memory block (0 since
        // this allocation is dedicated entirely to this one image).
        vkBindImageMemory(device_handle, depth_image, depth_image_memory, 0);

        // ---------- Image view creation ----------
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = depth_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = depth_format;

        // DEPTH_BIT here, not COLOR_BIT like the swapchain image views -
        // this view is for the depth aspect of the image, not color.
        // If the format includes a stencil component (e.g. D24_UNORM_S8_UINT),
        // you'd also include VK_IMAGE_ASPECT_STENCIL_BIT here once stencil
        // testing is actually used - left out for now since we don't use it yet.
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        result = vkCreateImageView(device_handle, &view_info, nullptr, &depth_image_view);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create depth image view: " + Vulkan_Utils::Vk_result_to_string(result)
            );
        }

        current_extent = _extent;

        std::cout << "[Vulkan_Depth_Resources] Depth resources created: "
            << _extent.width << "x" << _extent.height << "\n";
    }

    // ---------- Recreate ----------
    void Vulkan_Depth_Resources::Recreate(VkExtent2D _new_extent) {
        assert(depth_image != VK_NULL_HANDLE && "Recreate() called on a moved-from or already-destroyed Vulkan_Depth_Resources");
        assert(_new_extent.width > 0 && _new_extent.height > 0 && "Recreate() called with a zero extent");

        Destroy();
        Create(_new_extent);

        std::cout << "[Vulkan_Depth_Resources] Depth resources recreated: "
            << _new_extent.width << "x" << _new_extent.height << "\n";
    }

    // ---------- Move constructor ----------
    Vulkan_Depth_Resources::Vulkan_Depth_Resources(Vulkan_Depth_Resources&& _other) noexcept
        : device_handle(_other.device_handle),
        device(_other.device),
        depth_format(_other.depth_format),
        current_extent(_other.current_extent),
        depth_image(_other.depth_image),
        depth_image_memory(_other.depth_image_memory),
        depth_image_view(_other.depth_image_view) {

        _other.depth_image = VK_NULL_HANDLE;
        _other.depth_image_memory = VK_NULL_HANDLE;
        _other.depth_image_view = VK_NULL_HANDLE;
    }

    // ---------- Move assignment ----------
    Vulkan_Depth_Resources& Vulkan_Depth_Resources::operator=(Vulkan_Depth_Resources&& _other) noexcept {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            device = _other.device;
            depth_format = _other.depth_format;
            current_extent = _other.current_extent;
            depth_image = _other.depth_image;
            depth_image_memory = _other.depth_image_memory;
            depth_image_view = _other.depth_image_view;

            _other.depth_image = VK_NULL_HANDLE;
            _other.depth_image_memory = VK_NULL_HANDLE;
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