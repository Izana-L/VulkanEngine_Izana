#include <Texture_GPU.hpp>
#include <Vulkan_Barrier.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Utils.hpp>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace Renderer_System
{

    // ---------- Constructor ----------
    Texture_GPU::Texture_GPU(
        const Vulkan_Device&        _device,
        VmaAllocator                _allocator,
        Upload_Context&             _upload,
        VkCommandBuffer             _transfer_cmd,
        const CoreTypes::ImageData& _image_data,
        VkFormat                    _format)

        : device_handle(_device.Get_logical_device_handle()),
        allocator(_allocator),
        image(),
        image_view(),
        width(_image_data.width),
        height(_image_data.height),

        // Throws std::invalid_argument for a zero dimension, in every build,
        // before anything exists to release.
        mip_levels(Vulkan_Image_Utils::Compute_mip_levels(_image_data.width, _image_data.height)),
        format(_format)
    {
        assert(device_handle != VK_NULL_HANDLE &&
            "Vulkan_Device must be fully constructed before creating a Texture_GPU");
        assert(allocator != VK_NULL_HANDLE &&
            "Vulkan_Allocator must be fully constructed before creating a Texture_GPU");
        assert(_transfer_cmd != VK_NULL_HANDLE &&
            "Texture_GPU: transfer command buffer must be valid and already recording");

        // The image and its view are RAII members: an exception from here
        // releases whatever was created before it, and the staging buffers
        // are the Upload_Context's.
        Record_upload(_device, _upload, _transfer_cmd, _image_data);
    }

    // ---------- Record_upload ----------
    void Texture_GPU::Record_upload(const Vulkan_Device& _device, Upload_Context& _upload, VkCommandBuffer _transfer_cmd,
                                    const CoreTypes::ImageData& _image_data)
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
        // (ImageData.hpp). The levels it carries are uploaded as they are;
        // the rest of the chain, if any, is generated on the GPU from the
        // last one.
        const uint32_t provided_levels = _image_data.mip_levels;

        if (provided_levels == 0 || provided_levels > mip_levels) {
            throw std::invalid_argument(
                "Texture_GPU: ImageData declares " + std::to_string(provided_levels) +
                " mip level(s); a " + std::to_string(width) + "x" + std::to_string(height) +
                " image has between 1 and " + std::to_string(mip_levels));
        }

        // Where each level starts in the staging buffer, which is the pixel
        // data as it is.
        std::vector<Vulkan_Image_Utils::Mip_Copy_Region> level_copies;
        level_copies.reserve(provided_levels);

        VkDeviceSize expected_size = 0;
        for (uint32_t level = 0; level < provided_levels; ++level) {
            const uint32_t level_width = std::max(width >> level, 1u);
            const uint32_t level_height = std::max(height >> level, 1u);

            level_copies.push_back({ expected_size, level, level_width, level_height });
            expected_size += static_cast<VkDeviceSize>(level_width) * level_height * bytes_per_pixel;
        }

        // Exact size, not a lower bound: a buffer that holds more or fewer
        // bytes than the format needs was decoded with a different texel
        // layout (e.g. RGBA8 data declared as R8_UNORM), and uploading it
        // would produce a corrupt texture without any error.
        if (_image_data.pixels.size() != expected_size) {
            throw std::invalid_argument(
                "Texture_GPU: ImageData holds " + std::to_string(_image_data.pixels.size()) +
                " bytes but " + std::to_string(provided_levels) + " mip level(s) of a " +
                std::to_string(width) + "x" + std::to_string(height) +
                " image in format " + std::to_string(static_cast<int>(format)) +
                " take exactly " + std::to_string(expected_size));
        }

        // Every bindless slot may be read with any sampler preset, linear
        // ones included (Bindless_Sampled_Format_Features). TRANSFER_DST
        // for the buffer copy; the blit features and TRANSFER_SRC only
        // when levels of the chain are generated.
        const bool generate_mips = provided_levels < mip_levels;

        VkFormatFeatureFlags required_features = Vulkan_Image_Utils::Bindless_Sampled_Format_Features | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

        if (generate_mips)
            required_features |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;

        Vulkan_Image_Utils::Require_optimal_tiling_features(_device, format, required_features, "Texture_GPU");

        // =========================================================
        // Staging buffer
        // =========================================================

        // Owned by the Upload_Context of the transfer: it is freed with the
        // other staging buffers of the batch, once the copy has run.
        const Vulkan_Buffer_Utils::Buffer_Allocation staging = _upload.Create_staging(expected_size);

        Vulkan_Buffer_Utils::Upload_to_buffer(
            allocator,
            staging,
            _image_data.pixels.data(),
            expected_size
        );

        // =========================================================
        // Image creation
        // =========================================================

        // TRANSFER_SRC_BIT only when levels are generated: Generate_mipmaps
        // blits each mip level FROM the previous level of this same image,
        // so the image is both source and destination of those blits.
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        if (generate_mips)
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        // Throws before creating anything when the device cannot make an
        // image of this size and format (Require_image_support).
        image = Unique_Image(allocator, Vulkan_Image_Utils::Create_image(
            allocator,
            width, height,
            mip_levels,
            format,
            VK_IMAGE_TILING_OPTIMAL,
            usage
        ));

        // =========================================================
        // Upload + mipmap generation (recorded, not submitted here)
        // =========================================================

        // Transition the whole mip chain to TRANSFER_DST so the supplied
        // levels can be copied into. Generate_mipmaps re-transitions levels
        // individually as it processes them.
        Vulkan_Image_Utils::Transition_image_layout(
            _transfer_cmd, image.Get(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            { 0, mip_levels }
        );

        Vulkan_Image_Utils::Copy_buffer_to_image(
            _transfer_cmd, staging.buffer, image.Get(), level_copies
        );

        if (generate_mips)
        {
            // Generates the levels after the last supplied one, leaves every
            // level in SHADER_READ_ONLY_OPTIMAL when done.
            Vulkan_Image_Utils::Generate_mipmaps(
                _device, _transfer_cmd, image.Get(), format,
                static_cast<int32_t>(width), static_cast<int32_t>(height),
                mip_levels, provided_levels
            );
        }
        else
        {
            // Every level was supplied — no blitting needed, just transition
            // straight to shader-readable.
            Vulkan_Image_Utils::Transition_image_layout(
                _transfer_cmd, image.Get(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                { 0, mip_levels }
            );
        }

        // =========================================================
        // Image view
        // =========================================================

        image_view = Create_unique_image_view(
            device_handle, image.Get(), format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels
        );
    }

    // ---------- Getters ----------
    VkImageView Texture_GPU::Get_image_view() const
    {
        assert(image_view &&
            "Get_image_view() called on a moved-from or destroyed Texture_GPU");
        return image_view.Get();
    }

    VkImage Texture_GPU::Get_image() const
    {
        assert(image &&
            "Get_image() called on a moved-from or destroyed Texture_GPU");
        return image.Get();
    }

    uint32_t Texture_GPU::Get_width()      const { return width; }
    uint32_t Texture_GPU::Get_height()     const { return height; }
    uint32_t Texture_GPU::Get_mip_levels() const { return mip_levels; }
    VkFormat Texture_GPU::Get_format()     const { return format; }

} // namespace Renderer
