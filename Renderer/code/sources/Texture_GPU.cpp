#include <Texture_GPU.hpp>
#include <Vulkan_Image_Utils.hpp>
#include <Vulkan_Buffer_Utils.hpp>
#include <Vulkan_Utils.hpp>

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

        // =========================================================
        // Staging buffer
        // =========================================================

        // The staging size follows the FORMAT, not an assumed 4 bytes per
        // pixel, and the pixel buffer must actually hold that much: a copy
        // that read past the end of the vector would upload garbage or
        // crash. Block-compressed formats are rejected here because the
        // tightly packed copy and the blit-based mip generation below do
        // not apply to them.
        const uint32_t bytes_per_pixel = Vulkan_Image_Utils::Bytes_per_pixel(format);

        if (bytes_per_pixel == 0) {
            throw std::invalid_argument(
                "Texture_GPU: format " + std::to_string(static_cast<int>(format)) +
                " cannot be uploaded with a packed copy (block-compressed or unsupported)");
        }

        const VkDeviceSize image_size =
            static_cast<VkDeviceSize>(width) * height * bytes_per_pixel;

        if (_image_data.pixels.size() < image_size) {
            throw std::invalid_argument(
                "Texture_GPU: ImageData holds " + std::to_string(_image_data.pixels.size()) +
                " bytes but " + std::to_string(width) + "x" + std::to_string(height) +
                " texels of this format need " + std::to_string(image_size));
        }

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

        // TRANSFER_SRC_BIT is required even though this image is the upload
        // target, because Generate_mipmaps blits each mip level FROM the
        // previous level of this same image — the image is both source and
        // destination of those internal blits.
        image = Vulkan_Image_Utils::Create_image(
            allocator,
            width, height,
            mip_levels,
            format,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT
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

        if (mip_levels > 1)
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