#include <Storage_Image.hpp>

#include <stdexcept>
#include <string>
#include <iostream>
#include <cassert>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Storage_Image::Storage_Image(
        const Vulkan_Device& _device,
        VmaAllocator         _allocator,
        uint32_t             _width,
        uint32_t             _height,
        VkFormat             _format)

        : device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator),
        format(_format),
        extent{ _width, _height },
        image_view(VK_NULL_HANDLE),
        recorded_layout(VK_IMAGE_LAYOUT_UNDEFINED)
    {
        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a Storage_Image");
        assert(allocator != VK_NULL_HANDLE && "Vulkan_Allocator must be fully constructed before creating a Storage_Image");

        if (_width == 0 || _height == 0)
            throw std::invalid_argument("Storage_Image: width and height must be greater than zero");

        // ---------- Format support ----------
        // Enforced in every build: creating a STORAGE image with a format
        // that lacks the feature is invalid usage, and nothing guarantees
        // a validation layer is present to report it.
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(_device.Get_physical_device_handle(), _format, &format_properties);

        constexpr VkFormatFeatureFlags required_features = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

        if ((format_properties.optimalTilingFeatures & required_features) != required_features)
        {
            throw std::runtime_error("Storage_Image: format " + std::to_string(static_cast<int>(_format)) +
                " does not support storage and sampled use with optimal tiling on this device");
        }

        try
        {
            // ---------- Image creation ----------
            // STORAGE: written by compute shaders through imageStore.
            // SAMPLED: read through the bindless texture array.
            // One mip level, see the class comment.
            image = Vulkan_Image_Utils::Create_image(allocator, _width, _height, 1, _format, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

            // ---------- Image view creation ----------
            // A single view serves both uses: the view inherits the usage
            // of the image, and the layout is chosen per descriptor
            // (GENERAL for the storage descriptor, SHADER_READ_ONLY_OPTIMAL
            // for the bindless slot).
            image_view = Vulkan_Image_Utils::Create_image_view(device_handle, image.image, _format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }
        catch (...)
        {
            // The destructor does not run for a constructor that threw.
            Destroy();
            throw;
        }

        std::cout << "[Storage_Image] Created: " << _width << "x" << _height
            << ", format " << static_cast<int>(_format) << "\n";
    }

    // ---------- Destructor ----------
    Storage_Image::~Storage_Image()
    {
        Destroy();
    }

    // ---------- Destroy ----------
    void Storage_Image::Destroy()
    {
        // Reverse order of creation: the view depends on the image.
        if (image_view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_handle, image_view, nullptr);
            image_view = VK_NULL_HANDLE;
        }

        Vulkan_Image_Utils::Destroy_image(allocator, image);
    }

    // ---------- Move constructor ----------
    Storage_Image::Storage_Image(Storage_Image&& _other) noexcept
        : device_handle(_other.device_handle),
        allocator(_other.allocator),
        format(_other.format),
        extent(_other.extent),
        image(_other.image),
        image_view(_other.image_view),
        recorded_layout(_other.recorded_layout)
    {
        _other.image = {};
        _other.image_view = VK_NULL_HANDLE;
        _other.recorded_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // ---------- Move assignment ----------
    Storage_Image& Storage_Image::operator=(Storage_Image&& _other) noexcept
    {
        if (this != &_other)
        {
            Destroy();

            device_handle = _other.device_handle;
            allocator = _other.allocator;
            format = _other.format;
            extent = _other.extent;
            image = _other.image;
            image_view = _other.image_view;
            recorded_layout = _other.recorded_layout;

            _other.image = {};
            _other.image_view = VK_NULL_HANDLE;
            _other.recorded_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        return *this;
    }

    // ---------- Begin_write ----------
    void Storage_Image::Begin_write(VkCommandBuffer _command_buffer)
    {
        assert(image.image != VK_NULL_HANDLE && "Begin_write() called on a moved-from or destroyed Storage_Image");
        assert((recorded_layout == VK_IMAGE_LAYOUT_UNDEFINED || recorded_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) &&
            "Begin_write() called twice without End_write() in between");

        // The old layout is UNDEFINED even when the image is back in
        // SHADER_READ_ONLY_OPTIMAL: the dispatch rewrites every texel, so
        // the previous contents are discarded instead of preserved.
        Vulkan_Image_Utils::Transition_image_layout(_command_buffer, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 1);

        recorded_layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // ---------- End_write ----------
    void Storage_Image::End_write(VkCommandBuffer _command_buffer)
    {
        assert(image.image != VK_NULL_HANDLE && "End_write() called on a moved-from or destroyed Storage_Image");
        assert(recorded_layout == VK_IMAGE_LAYOUT_GENERAL && "End_write() called without a matching Begin_write()");

        Vulkan_Image_Utils::Transition_image_layout(_command_buffer, image.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);

        recorded_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // ---------- Getters ----------
    VkImage Storage_Image::Get_image() const
    {
        assert(image.image != VK_NULL_HANDLE && "Get_image() called on a moved-from or destroyed Storage_Image");
        return image.image;
    }

    VkImageView Storage_Image::Get_image_view() const
    {
        assert(image_view != VK_NULL_HANDLE && "Get_image_view() called on a moved-from or destroyed Storage_Image");
        return image_view;
    }

    VkFormat Storage_Image::Get_format() const
    {
        return format;
    }

    VkExtent2D Storage_Image::Get_extent() const
    {
        return extent;
    }

    VkImageLayout Storage_Image::Get_recorded_layout() const
    {
        return recorded_layout;
    }

}