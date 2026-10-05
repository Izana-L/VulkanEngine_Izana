#include <Texture_GPU.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Texture_GPU::Texture_GPU(
        const Vulkan_Device& _device,
        VmaAllocator                _allocator,
        VkCommandBuffer             _transfer_cmd,
        const CoreTypes::ImageData& _image_data,
        VkFormat                    _format)

        : device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator),
        image_view(VK_NULL_HANDLE),
        width(_image_data.width),
        height(_image_data.height),
        mip_levels(Vulkan_Image_Utils::Compute_mip_levels(_image_data.width, _image_data.height)),
        format(_format)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Texture_GPU");
        assert(allocator != VK_NULL_HANDLE &&
            "Vulkan_Allocator must be fully constructed before creating a Texture_GPU");
        assert(_transfer_cmd != VK_NULL_HANDLE &&
            "Texture_GPU: transfer command buffer must be valid and already recording");
        assert(!_image_data.pixels.empty() &&
            "Texture_GPU: ImageData has no pixel data");
        assert(width > 0 && height > 0 &&
            "Texture_GPU: ImageData has zero dimensions");

        // The destructor does not run for a constructor that throws, so
        // whatever was created before the failure (staging buffer, image,
        // view) is released here. Destroy() is safe on the null handles of
        // the steps that never ran. Commands already recorded into
        // _transfer_cmd reference the destroyed image, so the caller must
        // not submit that command buffer (Renderer::Upload_batch frees it).
        try
        {
            Record_upload(_device, _transfer_cmd, _image_data);
        }
        catch (...)
        {
            Destroy();
            throw;
        }
    }

    // ---------- Record_upload ----------
    void Texture_GPU::Record_upload(const Vulkan_Device& _device, VkCommandBuffer _transfer_cmd, const CoreTypes::ImageData& _image_data)
    {
        // =========================================================
        // Validation
        // =========================================================

        // The staging size follows the FORMAT, not an assumed 4 bytes per
        // pixel. Block-compressed formats are rejected here because the
        // tightly packed copy and the blit-based mip generation below do
        // not apply to them.
        const uint32_t bytes_per_pixel = Vulkan_Image_Utils::Bytes_per_pixel(format);

        if (bytes_per_pixel == 0) {
            throw std::invalid_argument(
                "Texture_GPU: format " + std::to_string(static_cast<int>(format)) +
                " cannot be uploaded with a packed copy (block-compressed or unsupported)");
        }

        // ImageData stores its mip levels consecutively, largest first
        // (ImageData.hpp). Only level 0 is uploaded; the rest of the chain
        // is generated on the GPU.
        if (_image_data.mip_levels == 0 || _image_data.mip_levels > mip_levels) {
            throw std::invalid_argument(
                "Texture_GPU: ImageData declares " + std::to_string(_image_data.mip_levels) +
                " mip level(s); a " + std::to_string(width) + "x" + std::to_string(height) +
                " image has between 1 and " + std::to_string(mip_levels));
        }

        const VkDeviceSize image_size =
            static_cast<VkDeviceSize>(width) * height * bytes_per_pixel;

        VkDeviceSize expected_size = 0;
        for (uint32_t level = 0; level < _image_data.mip_levels; ++level) {
            const VkDeviceSize level_width = std::max(width >> level, 1u);
            const VkDeviceSize level_height = std::max(height >> level, 1u);
            expected_size += level_width * level_height * bytes_per_pixel;
        }

        // Exact size, not a lower bound: a buffer that holds more or fewer
        // bytes than the format needs was decoded with a different texel
        // layout (e.g. RGBA8 data declared as R8_UNORM), and uploading it
        // would produce a corrupt texture without any error.
        if (_image_data.pixels.size() != expected_size) {
            throw std::invalid_argument(
                "Texture_GPU: ImageData holds " + std::to_string(_image_data.pixels.size()) +
                " bytes but " + std::to_string(_image_data.mip_levels) + " mip level(s) of a " +
                std::to_string(width) + "x" + std::to_string(height) +
                " image in format " + std::to_string(static_cast<int>(format)) +
                " take exactly " + std::to_string(expected_size));
        }

        // Every bindless slot may be read with any sampler preset, linear
        // ones included (Bindless_Sampled_Format_Features). TRANSFER_DST
        // for the buffer copy; the blit features and TRANSFER_SRC only
        // when a mip chain is generated.
        const bool generate_mips = mip_levels > 1;

        VkFormatFeatureFlags required_features = Vulkan_Image_Utils::Bindless_Sampled_Format_Features | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

        if (generate_mips)
            required_features |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;

        Vulkan_Image_Utils::Require_optimal_tiling_features(_device, format, required_features, "Texture_GPU");

        // =========================================================
        // Staging buffer
        // =========================================================

        staging = Vulkan_Buffer_Utils::Create_buffer(
            allocator,
            image_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            Vulkan_Buffer_Utils::Buffer_Access::Cpu_To_Gpu,
            true
        );

        Vulkan_Buffer_Utils::Upload_to_buffer(
            allocator,
            staging,
            _image_data.pixels.data(),
            image_size
        );

        // =========================================================
        // Image creation
        // =========================================================

        // TRANSFER_SRC_BIT only with a mip chain: Generate_mipmaps blits
        // each mip level FROM the previous level of this same image, so
        // the image is both source and destination of those blits.
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        if (generate_mips)
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        image = Vulkan_Image_Utils::Create_image(
            allocator,
            width, height,
            mip_levels,
            format,
            VK_IMAGE_TILING_OPTIMAL,
            usage
        );

        // =========================================================
        // Upload + mipmap generation (recorded, not submitted here)
        // =========================================================

        // Transition the whole mip chain to TRANSFER_DST so mip 0 can be
        // copied into. Generate_mipmaps re-transitions levels individually
        // as it processes them.
        Vulkan_Image_Utils::Transition_image_layout(
            _transfer_cmd, image.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            mip_levels
        );

        Vulkan_Image_Utils::Copy_buffer_to_image(
            _transfer_cmd, staging.buffer, image.image, width, height
        );

        if (generate_mips)
        {
            // Generates mips 1..N from mip 0, leaves every level in
            // SHADER_READ_ONLY_OPTIMAL when done.
            Vulkan_Image_Utils::Generate_mipmaps(
                _device, _transfer_cmd, image.image, format,
                static_cast<int32_t>(width), static_cast<int32_t>(height),
                mip_levels
            );
        }
        else
        {
            // Single mip level — no blitting needed, just transition
            // straight to shader-readable.
            Vulkan_Image_Utils::Transition_image_layout(
                _transfer_cmd, image.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                mip_levels
            );
        }

        // =========================================================
        // Image view
        // =========================================================

        image_view = Vulkan_Image_Utils::Create_image_view(
            device_handle, image.image, format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels
        );
    }

    // ---------- Destructor ----------
    Texture_GPU::~Texture_GPU()
    {
        Destroy();
    }

    // ---------- Destroy ----------
    void Texture_GPU::Destroy()
    {
        Release_staging_buffers();

        if (image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_handle, image_view, nullptr);
            image_view = VK_NULL_HANDLE;
        }
        Vulkan_Image_Utils::Destroy_image(allocator, image);
    }

    // ---------- Release_staging_buffers ----------
    void Texture_GPU::Release_staging_buffers()
    {
        Vulkan_Buffer_Utils::Destroy_buffer(allocator, staging);
    }

    // ---------- Move constructor ----------
    Texture_GPU::Texture_GPU(Texture_GPU&& _other) noexcept
        : device_handle(_other.device_handle),
        allocator(_other.allocator),
        image(_other.image),
        image_view(_other.image_view),
        staging(_other.staging),
        width(_other.width),
        height(_other.height),
        mip_levels(_other.mip_levels),
        format(_other.format)
    {
        _other.image = {};
        _other.image_view = VK_NULL_HANDLE;
        _other.staging = {};
    }

    // ---------- Move assignment ----------
    Texture_GPU& Texture_GPU::operator=(Texture_GPU&& _other) noexcept
    {
        if (this != &_other) {
            Destroy();

            device_handle = _other.device_handle;
            allocator = _other.allocator;
            image = _other.image;
            image_view = _other.image_view;
            staging = _other.staging;
            width = _other.width;
            height = _other.height;
            mip_levels = _other.mip_levels;
            format = _other.format;

            _other.image = {};
            _other.image_view = VK_NULL_HANDLE;
            _other.staging = {};
        }
        return *this;
    }

    // ---------- Getters ----------
    VkImageView Texture_GPU::Get_image_view() const
    {
        assert(image_view != VK_NULL_HANDLE &&
            "Get_image_view() called on a moved-from or destroyed Texture_GPU");
        return image_view;
    }

    VkImage Texture_GPU::Get_image() const
    {
        assert(image.image != VK_NULL_HANDLE &&
            "Get_image() called on a moved-from or destroyed Texture_GPU");
        return image.image;
    }

    uint32_t Texture_GPU::Get_width()      const { return width; }
    uint32_t Texture_GPU::Get_height()     const { return height; }
    uint32_t Texture_GPU::Get_mip_levels() const { return mip_levels; }
    VkFormat Texture_GPU::Get_format()     const { return format; }

} // namespace Renderer