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
        VkCommandBuffer      _command_buffer,
        uint32_t             _width,
        uint32_t             _height,
        VkFormat             _format,
        VkPipelineStageFlags _reader_stages)

        : device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator),
        format(_format),
        extent{ _width, _height },
        reader_stages(_reader_stages),
        image_view(VK_NULL_HANDLE),
        recorded_layout(VK_IMAGE_LAYOUT_UNDEFINED)
    {
        assert(device_handle != VK_NULL_HANDLE && "Vulkan_Device must be fully constructed before creating a Storage_Image");
        assert(allocator != VK_NULL_HANDLE && "Vulkan_Allocator must be fully constructed before creating a Storage_Image");
        assert(_command_buffer != VK_NULL_HANDLE && "Storage_Image: the command buffer for the initial clear must be valid and recording");

        if (_width == 0 || _height == 0)
            throw std::invalid_argument("Storage_Image: width and height must be greater than zero");

        // Enforced in every build: the reader stages become stage masks of
        // every barrier of this image. An empty mask is invalid usage, and
        // a stage outside the bindless reader stages cannot read the slot,
        // so its SHADER_READ access would be meaningless or unsupported.
        if (_reader_stages == 0 || (_reader_stages & ~Bindless_Reader_Pipeline_Stages) != 0)
        {
            throw std::invalid_argument("Storage_Image: _reader_stages must be a non-empty subset of "
                                        "Bindless_Reader_Pipeline_Stages");
        }

        // ---------- Format support ----------
        // Enforced in every build: creating a STORAGE image with a format
        // that lacks the feature is invalid usage, and nothing guarantees
        // a validation layer is present to report it. TRANSFER_DST is
        // required by the initial clear. The image is read through its
        // bindless slot, and any sampler preset may read any slot, so the
        // format also needs the features of every bindless texture
        // (Bindless_Sampled_Format_Features), linear filtering included:
        // without it, a Linear_* preset reading the image is invalid.
        constexpr VkFormatFeatureFlags required_features = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                                           VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                                           Vulkan_Image_Utils::Bindless_Sampled_Format_Features;

        Vulkan_Image_Utils::Require_optimal_tiling_features(_device, _format, required_features, "Storage_Image");

        try
        {
            Create_image_and_view(extent, image, image_view);

            // Part of construction, so the image is never observable
            // before it holds defined contents in its declared layout.
            Record_initial_clear(_command_buffer);
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

    // ---------- Create_image_and_view ----------
    void Storage_Image::Create_image_and_view(VkExtent2D _extent,
        Vulkan_Image_Utils::Image_Allocation& _out_image,
        VkImageView&                          _out_view) const
    {
        // ---------- Image creation ----------
        // STORAGE:      written by compute shaders through imageStore.
        // SAMPLED:      read through the bindless texture array.
        // TRANSFER_DST: target of the initial clear (vkCmdClearColorImage).
        // One mip level, see the class comment.
        Vulkan_Image_Utils::Image_Allocation new_image = Vulkan_Image_Utils::Create_image(allocator, _extent.width, _extent.height, 1,
            format, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        // ---------- Image view creation ----------
        // A single view serves both uses: the view inherits the usage
        // of the image, and the layout is chosen per descriptor
        // (GENERAL for the storage descriptor, SHADER_READ_ONLY_OPTIMAL
        // for the bindless slot).
        VkImageView new_view = VK_NULL_HANDLE;

        try
        {
            new_view = Vulkan_Image_Utils::Create_image_view(device_handle, new_image.image, format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }
        catch (...)
        {
            Vulkan_Image_Utils::Destroy_image(allocator, new_image);
            throw;
        }

        _out_image = new_image;
        _out_view = new_view;
    }

    // ---------- Record_initial_clear ----------
    void Storage_Image::Record_initial_clear(VkCommandBuffer _command_buffer)
    {
        using Vulkan_Image_Utils::Barrier_Scope;

        // The image was just created: no earlier command accesses it, so
        // the clear has nothing to wait for.
        const Barrier_Scope nothing_before{ VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0 };
        const Barrier_Scope clear_write{ VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
        const Barrier_Scope readers{ reader_stages, VK_ACCESS_SHADER_READ_BIT };

        Vulkan_Image_Utils::Record_image_barrier(_command_buffer, image.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nothing_before, clear_write, 1);

        // Value-initialization zeroes the whole union: 0.0 for float and
        // normalized formats, 0 for integer ones.
        const VkClearColorValue zero{};

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        vkCmdClearColorImage(_command_buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);

        // The cleared contents become visible to every reader stage, and
        // the image enters the declared layout of its bindless slot
        // (Bindless_Registry::Register_texture).
        Vulkan_Image_Utils::Record_image_barrier(_command_buffer, image.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, clear_write, readers, 1);

        recorded_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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

    // ---------- Recreate ----------
    void Storage_Image::Recreate(VkCommandBuffer _command_buffer, VkExtent2D _new_extent)
    {
        assert(image.image != VK_NULL_HANDLE && "Recreate() called on a moved-from or destroyed Storage_Image");
        assert(_command_buffer != VK_NULL_HANDLE && "Recreate() called with a null command buffer");
        assert(recorded_layout != VK_IMAGE_LAYOUT_GENERAL && "Recreate() called between Begin_write() and End_write()");

        if (_new_extent.width == 0 || _new_extent.height == 0)
            throw std::invalid_argument("Storage_Image::Recreate: width and height must be greater than zero");

        // The new image exists before the old one is destroyed: if its
        // creation throws, the old image, its view and the bindless slot
        // that points at the view all stay valid.
        Vulkan_Image_Utils::Image_Allocation new_image{};
        VkImageView new_view = VK_NULL_HANDLE;
        Create_image_and_view(_new_extent, new_image, new_view);

        Destroy();

        image = new_image;
        image_view = new_view;
        extent = _new_extent;

        // Cannot throw: the stage masks were validated at construction.
        Record_initial_clear(_command_buffer);

        std::cout << "[Storage_Image] Recreated: " << _new_extent.width << "x" << _new_extent.height
            << ", format " << static_cast<int>(format) << "\n";
    }

    // ---------- Move constructor ----------
    Storage_Image::Storage_Image(Storage_Image&& _other) noexcept
        : device_handle(_other.device_handle),
        allocator(_other.allocator),
        format(_other.format),
        extent(_other.extent),
        reader_stages(_other.reader_stages),
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
            reader_stages = _other.reader_stages;
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
        assert(recorded_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            "Begin_write() called twice without End_write() in between");

        // The old layout is UNDEFINED even though the image is in
        // SHADER_READ_ONLY_OPTIMAL: the dispatch rewrites every texel, so
        // the previous contents are discarded instead of preserved.
        //
        // Source: the reader stages, not TOP_OF_PIPE. The reads of earlier
        // frames must finish before the image is overwritten
        // (write-after-read). A barrier orders against every command
        // submitted earlier to the same queue, so this also covers frames
        // recorded in other command buffers. A write-after-read hazard
        // needs only an execution dependency, hence no source access. The
        // earlier write (the initial clear or the previous dispatch) was
        // made available by the barrier that ended it, whose destination is
        // these same stages, so the dependency chains through them.
        //
        // Destination: the compute shader writes of the dispatch.
        Vulkan_Image_Utils::Record_image_barrier(_command_buffer, image.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            { reader_stages, 0 },
            { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT },
            1);

        recorded_layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // ---------- End_write ----------
    void Storage_Image::End_write(VkCommandBuffer _command_buffer)
    {
        assert(image.image != VK_NULL_HANDLE && "End_write() called on a moved-from or destroyed Storage_Image");
        assert(recorded_layout == VK_IMAGE_LAYOUT_GENERAL && "End_write() called without a matching Begin_write()");

        // Source: the compute shader writes, made available. Destination:
        // the reader stages, which the writes are made visible to, with the
        // image back in the declared layout of its bindless slot
        // (Bindless_Registry::Register_texture).
        Vulkan_Image_Utils::Record_image_barrier(_command_buffer, image.image,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT },
            { reader_stages, VK_ACCESS_SHADER_READ_BIT },
            1);

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

    VkPipelineStageFlags Storage_Image::Get_reader_stages() const
    {
        return reader_stages;
    }

    VkImageLayout Storage_Image::Get_recorded_layout() const
    {
        return recorded_layout;
    }

}
